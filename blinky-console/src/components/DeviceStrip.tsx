/**
 * DeviceStrip — horizontal target picker at the top of MainShell.
 *
 * One pill for "All devices" (fleet) plus one per discovered device. Tap to
 * scope every control below to that target. No routing — pure selection state.
 */
import type { Device } from '../services/sources';

interface DeviceStripProps {
  devices: Device[];
  /** null = fleet ("All"), string = a specific device id. */
  selectedId: string | null;
  onSelect: (id: string | null) => void;
  /** True if the same-origin blinky-server is reachable (enables fleet mode). */
  serverReachable: boolean;
}

export function DeviceStrip({ devices, selectedId, onSelect, serverReachable }: DeviceStripProps) {
  const connectedCount = devices.filter(d => d.isConnected()).length;
  const totalCount = devices.length;

  return (
    <nav className="device-strip" aria-label="Target selection">
      <button
        type="button"
        role="radio"
        aria-checked={selectedId === null}
        className={`device-strip__pill device-strip__pill--all ${
          selectedId === null ? 'active' : ''
        }`}
        onClick={() => onSelect(null)}
        disabled={!serverReachable}
        title={
          serverReachable
            ? 'Apply to every connected device'
            : 'Fleet mode requires blinky-server (same-origin)'
        }
      >
        <span className="device-strip__label">All</span>
        <span className="device-strip__count">
          {connectedCount}/{totalCount}
        </span>
      </button>

      {devices.map(d => {
        const connected = d.isConnected();
        const isSelected = selectedId === d.id;
        return (
          <button
            key={d.id}
            type="button"
            role="radio"
            aria-checked={isSelected}
            className={`device-strip__pill ${isSelected ? 'active' : ''} ${
              connected ? '' : 'device-strip__pill--offline'
            }`}
            onClick={() => onSelect(d.id)}
            title={`${d.displayName} · ${connected ? 'connected' : 'offline'}`}
          >
            <span
              className={`status-dot ${connected ? 'status-dot--on' : ''}`}
              aria-hidden="true"
            />
            <span className="device-strip__label">{d.displayName}</span>
          </button>
        );
      })}
    </nav>
  );
}
