import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { FirmwarePage } from './FirmwarePage';
import * as releases from '../services/firmware/releases';
import * as ota from '../services/firmware/otaFlash';
import * as usb from '../services/firmware/usbFlash';
import type { OtaRelease, ReleaseEntry, ReleaseIndex } from '../services/firmware/releases';

vi.mock('../services/firmware/releases', async importOriginal => ({
  ...(await importOriginal<typeof releases>()),
  fetchReleaseIndex: vi.fn(),
  loadOtaRelease: vi.fn(),
  loadVerifiedRelease: vi.fn(),
}));
vi.mock('../services/firmware/otaFlash', async importOriginal => ({
  ...(await importOriginal<typeof ota>()),
  bleUpdateSupported: () => true,
  updateDevice: vi.fn(),
}));
vi.mock('../services/firmware/usbFlash', async importOriginal => ({
  ...(await importOriginal<typeof usb>()),
  usbFlashSupported: () => true,
  recoverOverUsb: vi.fn(),
}));

function release(tag: string, signed = true): ReleaseEntry {
  const file = (ext: string) => ({ path: `${tag}/f.${ext}`, sha256: '0'.repeat(64) });
  return {
    tag,
    build: Number(tag.slice(1)),
    version: `${tag}-aaaaaaaa`,
    git_sha: 'a'.repeat(40),
    settings_version: 70,
    device_version: 1,
    published_at: '2026-10-01T00:00:00Z',
    url: `https://github.com/x/y/releases/tag/${tag}`,
    files: signed
      ? { hex: file('hex'), uf2: file('uf2'), bin: file('bin'), otam: file('otam') }
      : { hex: file('hex'), uf2: file('uf2') },
  };
}

// b217 is newer but not Latest; b213 predates signed updates.
const index: ReleaseIndex = {
  schema: 1,
  latest: 'b216',
  releases: [release('b217'), release('b216'), release('b213', false)],
};

const options = () =>
  Array.from((screen.getByRole('combobox') as HTMLSelectElement).options, o => o.value);

beforeEach(() => vi.clearAllMocks());

describe('FirmwarePage', () => {
  it('defaults to the latest stable release and offers only signed releases for updates', async () => {
    vi.mocked(releases.fetchReleaseIndex).mockResolvedValue(index);
    render(<FirmwarePage />);
    expect(await screen.findByRole('button', { name: 'Update to b216' })).toBeInTheDocument();
    expect((screen.getByRole('combobox') as HTMLSelectElement).value).toBe('b216');
    expect(options()).toEqual(['b217', 'b216']);
  });

  it('offers every release, signed or not, for recovery over USB', async () => {
    vi.mocked(releases.fetchReleaseIndex).mockResolvedValue(index);
    render(<FirmwarePage />);
    await screen.findByRole('button', { name: 'Update to b216' });
    fireEvent.click(screen.getByRole('checkbox', { name: /stuck in update mode/ }));
    expect(options()).toEqual(['b217', 'b216', 'b213']);
    expect(screen.queryByRole('radiogroup', { name: 'Connection' })).not.toBeInTheDocument();
  });

  it('says so when no published release supports browser updates yet', async () => {
    vi.mocked(releases.fetchReleaseIndex).mockResolvedValue({
      schema: 1,
      latest: 'b213',
      releases: [release('b213', false)],
    });
    render(<FirmwarePage />);
    expect(await screen.findByText(/released before signed updates/)).toBeInTheDocument();
    expect(screen.getByRole('button', { name: /Update to/ })).toBeDisabled();
  });

  it('refuses to offer an update when no stable release exists', async () => {
    vi.mocked(releases.fetchReleaseIndex).mockResolvedValue({
      schema: 1,
      latest: null,
      releases: [],
    });
    render(<FirmwarePage />);
    expect(await screen.findByText(/No stable firmware release/)).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: /Update to/ })).not.toBeInTheDocument();
  });

  it('shows why the release list could not load', async () => {
    vi.mocked(releases.fetchReleaseIndex).mockRejectedValue(new Error('HTTP 500'));
    render(<FirmwarePage />);
    expect(
      await screen.findByText(/Could not load the release list: HTTP 500/)
    ).toBeInTheDocument();
  });

  it('never reaches the device when the update files fail verification', async () => {
    vi.mocked(releases.fetchReleaseIndex).mockResolvedValue(index);
    vi.mocked(releases.loadOtaRelease).mockRejectedValue(new Error('SHA-512 mismatch'));
    render(<FirmwarePage />);
    fireEvent.click(await screen.findByRole('button', { name: 'Update to b216' }));
    await waitFor(() =>
      expect(
        screen.getAllByText(/did not pass their checks.*SHA-512 mismatch/).length
      ).toBeGreaterThan(0)
    );
    expect(ota.updateDevice).not.toHaveBeenCalled();
  });

  it('updates over the chosen connection', async () => {
    vi.mocked(releases.fetchReleaseIndex).mockResolvedValue(index);
    const rel = { entry: release('b216'), image: new Uint8Array(4) } as unknown as OtaRelease;
    vi.mocked(releases.loadOtaRelease).mockResolvedValue(rel);
    vi.mocked(ota.updateDevice).mockResolvedValue({
      version: 'b216-aaaaaaaa',
      device: { configured: true, id: 'hat_v1', name: 'Hat' },
    } as never);
    render(<FirmwarePage />);
    await screen.findByRole('button', { name: 'Update to b216' });
    fireEvent.click(screen.getByRole('radio', { name: /Bluetooth/ }));
    fireEvent.click(screen.getByRole('button', { name: 'Update to b216' }));
    expect(await screen.findByText(/Updated: Hat is running b216-aaaaaaaa/)).toBeInTheDocument();
    expect(vi.mocked(ota.updateDevice).mock.calls[0][0]).toBe('ble');
  });

  it('Cancel during the download stops before any device interaction', async () => {
    vi.mocked(releases.fetchReleaseIndex).mockResolvedValue(index);
    let finishDownload: (r: OtaRelease) => void = () => {};
    vi.mocked(releases.loadOtaRelease).mockReturnValue(
      new Promise(resolve => {
        finishDownload = resolve;
      })
    );
    render(<FirmwarePage />);
    fireEvent.click(await screen.findByRole('button', { name: 'Update to b216' }));
    fireEvent.click(await screen.findByRole('button', { name: 'Cancel' }));
    finishDownload({ entry: release('b216'), image: new Uint8Array(4) } as unknown as OtaRelease);
    await waitFor(() => expect(screen.getAllByText('Cancelled').length).toBeGreaterThan(0));
    expect(ota.updateDevice).not.toHaveBeenCalled();
  });
});
