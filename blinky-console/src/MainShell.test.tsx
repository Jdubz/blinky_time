/**
 * MainShell tests — minimal coverage for the three target shapes that
 * `dispatchEnabled` and the warning banner branch on:
 *   1. fleet  — no device selected, server reachable
 *   2. device — selected device with a connected protocol
 *   3. device-unavailable — selected device but no connected protocol
 *
 * Per PR 131 review: these branches were untested and their interaction
 * with `dispatchEnabled` is the root reason the previous version silently
 * fell back to fleet commands when the device wasn't ready. Added
 * 2026-04-24.
 */
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render, screen, fireEvent } from '@testing-library/react';
import { MainShell } from './MainShell';
import { Device } from './services/sources';
import type { DeviceProtocol } from './services/protocol';

// Mock useDevices so each test controls (devices, serverUrl) without
// touching the real registry / same-origin probe.
const mockUseDevices = vi.fn();
vi.mock('./hooks/useDevices', () => ({
  useDevices: () => mockUseDevices(),
}));

// Mock target dispatch so we can assert which shape was used without a
// real network or DeviceProtocol.
const mockSetGenerator = vi.fn().mockResolvedValue(undefined);
vi.mock('./lib/target', async () => {
  const actual = await vi.importActual<typeof import('./lib/target')>('./lib/target');
  return {
    ...actual,
    targetSetGenerator: (...args: unknown[]) => mockSetGenerator(...args),
    targetSetEffect: vi.fn().mockResolvedValue(undefined),
    targetSetSetting: vi.fn().mockResolvedValue(undefined),
  };
});

// ScenesPanel hits /api/scenes on mount; stub fetch so the test doesn't
// produce an unhandled rejection log.
beforeEach(() => {
  mockSetGenerator.mockClear();
  mockUseDevices.mockReset();
  vi.stubGlobal(
    'fetch',
    vi.fn().mockResolvedValue({ ok: true, json: () => Promise.resolve([]) } as Response)
  );
});

function makeConnectedDevice(id = 'dev-1', name = 'Kitchen'): Device {
  const proto = {
    isConnected: () => true,
    setGenerator: vi.fn().mockResolvedValue(undefined),
    setEffect: vi.fn().mockResolvedValue(undefined),
    setSetting: vi.fn().mockResolvedValue(undefined),
  } as unknown as DeviceProtocol;
  return new Device(id, name, [], proto);
}

function makeDisconnectedDevice(id = 'dev-2', name = 'Garage'): Device {
  // No transports and no protocol → init effect bails out and protocol
  // stays null, so isConnected() returns false. This is the
  // device-unavailable branch.
  return new Device(id, name, [], null);
}

describe('MainShell target shapes', () => {
  it('fleet: subtitle shows "All", generator dispatches with fleet target', async () => {
    mockUseDevices.mockReturnValue({ devices: [], serverUrl: 'http://localhost' });
    render(<MainShell />);
    expect(screen.getByText(/All ·/)).toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: /water/i }));
    await vi.waitFor(() => expect(mockSetGenerator).toHaveBeenCalled());
    const [target] = mockSetGenerator.mock.calls[0];
    expect((target as { kind: string }).kind).toBe('fleet');
  });

  it('fleet without server: warning banner shown, dispatch disabled', () => {
    mockUseDevices.mockReturnValue({ devices: [], serverUrl: null });
    render(<MainShell />);
    expect(screen.getByText(/blinky-server not detected/i)).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: /water/i }));
    expect(mockSetGenerator).not.toHaveBeenCalled();
  });

  it('device-unavailable: selecting a disconnected device shows warning and blocks dispatch', () => {
    const dev = makeDisconnectedDevice();
    mockUseDevices.mockReturnValue({ devices: [dev], serverUrl: 'http://localhost' });
    render(<MainShell />);

    // DeviceStrip renders the device pill; click to select it.
    fireEvent.click(screen.getByRole('radio', { name: /Garage/i }));
    expect(screen.getByText(/Garage is not connected/i)).toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: /water/i }));
    expect(mockSetGenerator).not.toHaveBeenCalled();
  });

  it('device connected: dispatches with device target, not fleet', async () => {
    const dev = makeConnectedDevice();
    mockUseDevices.mockReturnValue({ devices: [dev], serverUrl: 'http://localhost' });
    render(<MainShell />);

    fireEvent.click(screen.getByRole('radio', { name: /Kitchen/i }));
    fireEvent.click(screen.getByRole('button', { name: /water/i }));

    await vi.waitFor(() => expect(mockSetGenerator).toHaveBeenCalled());
    const [target] = mockSetGenerator.mock.calls[0];
    expect((target as { kind: string }).kind).toBe('device');
  });
});
