import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render, screen, fireEvent } from '@testing-library/react';
import { AudioDebugPage } from './AudioDebugPage';
import { Device } from '../services/sources/types';
import type { Source, TransportBinding } from '../services/sources/types';
import type { Transport } from '../services/transport';
import type { DeviceProtocol, SerialEventCallback } from '../services/protocol';

// Stub Chart.js so jsdom doesn't choke on canvas.
vi.mock('react-chartjs-2', () => ({
  Line: vi.fn(({ ref }) => {
    if (ref) {
      ref.current = {
        data: { labels: [], datasets: [{ data: [] }, { data: [] }, { data: [] }, { data: [] }] },
        update: vi.fn(),
      };
    }
    return <div data-testid="mock-chart">Chart</div>;
  }),
}));

// MainShell's lazy DeviceProtocol init mirrors what AudioDebugPage does — but
// we don't want a real protocol firing connect() against a fake transport
// inside this page either. Stub the constructor to just hand back a shell.
vi.mock('../services/protocol', () => {
  class StubProtocol {
    listeners: SerialEventCallback[] = [];
    isConnected() {
      return false;
    }
    addEventListener(cb: SerialEventCallback) {
      this.listeners.push(cb);
    }
    removeEventListener(cb: SerialEventCallback) {
      const i = this.listeners.indexOf(cb);
      if (i >= 0) this.listeners.splice(i, 1);
    }
    connect() {
      return Promise.resolve(true);
    }
    setStreamEnabled() {
      return Promise.resolve();
    }
  }
  return { DeviceProtocol: StubProtocol };
});

const fakeSource = (kind: Source['kind'] = 'blinky-server'): Source => ({
  kind,
  displayName: `${kind} fake`,
  isSupported: () => true,
});

const fakeTransport = (): Transport =>
  ({
    isSupported: () => true,
    isConnected: () => false,
    connect: vi.fn(() => Promise.resolve()),
    disconnect: vi.fn(),
    send: vi.fn(),
    addEventListener: vi.fn(),
    removeEventListener: vi.fn(),
  }) as unknown as Transport;

const fakeBinding = (kind: Source['kind'] = 'blinky-server'): TransportBinding => ({
  source: fakeSource(kind),
  transport: fakeTransport(),
});

function makeDevice(id: string, name: string, protocol: DeviceProtocol | null = null): Device {
  return new Device(id, name, [fakeBinding()], protocol);
}

describe('AudioDebugPage', () => {
  beforeEach(() => {
    vi.clearAllMocks();
  });

  it('renders the device picker with all devices', () => {
    const devices = [makeDevice('SN-A', 'Bucket'), makeDevice('SN-B', 'Tube')];

    render(<AudioDebugPage devices={devices} onClose={() => {}} />);

    const select = screen.getByRole('combobox') as HTMLSelectElement;
    expect(select).toBeInTheDocument();
    // Default placeholder + one per device.
    expect(select.options).toHaveLength(3);
    expect(screen.getByRole('option', { name: /Bucket/ })).toBeInTheDocument();
    expect(screen.getByRole('option', { name: /Tube/ })).toBeInTheDocument();
  });

  it('shows placeholder text when no device is selected', () => {
    render(<AudioDebugPage devices={[makeDevice('SN-A', 'Bucket')]} onClose={() => {}} />);

    expect(
      screen.getByText('Pick a device above to start previewing its audio stream.')
    ).toBeInTheDocument();
    expect(screen.queryByTestId('mock-chart')).not.toBeInTheDocument();
  });

  it('renders the AudioVisualizer once a device is selected', () => {
    const devices = [makeDevice('SN-A', 'Bucket')];

    render(<AudioDebugPage devices={devices} onClose={() => {}} />);

    const select = screen.getByRole('combobox');
    fireEvent.change(select, { target: { value: 'SN-A' } });

    expect(screen.getByTestId('mock-chart')).toBeInTheDocument();
    expect(screen.getByText('AdaptiveMic Output')).toBeInTheDocument();
    expect(
      screen.queryByText('Pick a device above to start previewing its audio stream.')
    ).not.toBeInTheDocument();
  });

  it('lazily creates a DeviceProtocol on the picked device', () => {
    const device = makeDevice('SN-A', 'Bucket');
    expect(device.protocol).toBeNull();

    render(<AudioDebugPage devices={[device]} onClose={() => {}} />);

    fireEvent.change(screen.getByRole('combobox'), { target: { value: 'SN-A' } });

    expect(device.protocol).not.toBeNull();
  });

  it('reuses an existing protocol on the device rather than overwriting it', () => {
    // If a protocol is already bound (e.g. by MainShell), AudioDebugPage must
    // not stamp a new one over it — that would lose the existing connection.
    const fakeProto = {
      isConnected: () => true,
      addEventListener: vi.fn(),
      removeEventListener: vi.fn(),
      connect: vi.fn(() => Promise.resolve(true)),
      setStreamEnabled: vi.fn(() => Promise.resolve()),
    } as unknown as DeviceProtocol;
    const device = makeDevice('SN-A', 'Bucket', fakeProto);

    render(<AudioDebugPage devices={[device]} onClose={() => {}} />);
    fireEvent.change(screen.getByRole('combobox'), { target: { value: 'SN-A' } });

    expect(device.protocol).toBe(fakeProto);
  });

  it('calls onClose when the Back button is clicked', () => {
    const onClose = vi.fn();
    render(<AudioDebugPage devices={[]} onClose={onClose} />);

    fireEvent.click(screen.getByRole('button', { name: /Back/ }));
    expect(onClose).toHaveBeenCalledTimes(1);
  });

  it('shows disconnected status when no device is selected', () => {
    render(<AudioDebugPage devices={[makeDevice('SN-A', 'Bucket')]} onClose={() => {}} />);
    expect(screen.getByText('disconnected')).toBeInTheDocument();
  });

  it('shows connecting status while the protocol is opening', () => {
    render(<AudioDebugPage devices={[makeDevice('SN-A', 'Bucket')]} onClose={() => {}} />);

    fireEvent.change(screen.getByRole('combobox'), { target: { value: 'SN-A' } });

    // Stub protocol's isConnected returns false, so badge stays in "connecting".
    expect(screen.getByText('connecting')).toBeInTheDocument();
  });

  it('shows the source kind alongside the device name in the picker', () => {
    const devices = [makeDevice('SN-A', 'Bucket')];

    render(<AudioDebugPage devices={devices} onClose={() => {}} />);

    expect(screen.getByRole('option', { name: /Bucket.*blinky-server/ })).toBeInTheDocument();
  });
});
