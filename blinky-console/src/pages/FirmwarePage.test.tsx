import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { FirmwarePage } from './FirmwarePage';
import * as releases from '../services/firmware/releases';
import * as usb from '../services/firmware/usbFlash';
import type { ReleaseEntry, ReleaseIndex } from '../services/firmware/releases';

vi.mock('../services/firmware/releases', async importOriginal => ({
  ...(await importOriginal<typeof releases>()),
  fetchReleaseIndex: vi.fn(),
  loadVerifiedRelease: vi.fn(),
}));
vi.mock('../services/firmware/usbFlash', async importOriginal => ({
  ...(await importOriginal<typeof usb>()),
  usbFlashSupported: () => true,
  flashOverUsb: vi.fn(),
}));

function release(tag: string): ReleaseEntry {
  const file = { path: `${tag}/f`, sha256: '0'.repeat(64) };
  return {
    tag,
    build: Number(tag.slice(1)),
    version: `${tag}-aaaaaaaa`,
    git_sha: 'a'.repeat(40),
    settings_version: 70,
    device_version: 1,
    published_at: '2026-10-01T00:00:00Z',
    url: `https://github.com/x/y/releases/tag/${tag}`,
    files: { hex: file, uf2: file },
  };
}

const index: ReleaseIndex = {
  schema: 1,
  latest: 'b216',
  releases: [release('b217'), release('b216'), release('b214')],
};

beforeEach(() => vi.clearAllMocks());

describe('FirmwarePage', () => {
  it('defaults to the latest stable release, not the newest listed', async () => {
    vi.mocked(releases.fetchReleaseIndex).mockResolvedValue(index);
    render(<FirmwarePage />);
    expect(await screen.findByRole('button', { name: 'Update to b216' })).toBeInTheDocument();
    expect((screen.getByRole('combobox') as HTMLSelectElement).value).toBe('b216');
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

  it('never reaches the device when the firmware files fail verification', async () => {
    vi.mocked(releases.fetchReleaseIndex).mockResolvedValue(index);
    vi.mocked(releases.loadVerifiedRelease).mockRejectedValue(new Error('sha256 mismatch'));
    render(<FirmwarePage />);
    fireEvent.click(await screen.findByRole('button', { name: 'Update to b216' }));
    await waitFor(() =>
      expect(
        screen.getAllByText(/did not pass their checks.*sha256 mismatch/).length
      ).toBeGreaterThan(0)
    );
    expect(usb.flashOverUsb).not.toHaveBeenCalled();
  });
});
