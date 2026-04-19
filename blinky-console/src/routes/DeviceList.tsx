/**
 * DeviceList — shows all discovered devices across all sources.
 *
 * When only one device is available, auto-navigates to it (preserving
 * the single-device UX from before routing was added). With multiple
 * devices, shows cards with connection status and transport info.
 */

import { useEffect, useRef } from 'react';
import { useNavigate } from 'react-router-dom';
import { useDevices } from '../hooks/useDevices';
import { WebSerialSource, deviceRegistry } from '../services/sources';
import type { Device } from '../services/sources';

function DeviceCard({ device }: { device: Device }) {
  const navigate = useNavigate();
  const isConnected = device.isConnected();
  const transportLabels = device.transports.map((t) => t.source.kind).join(', ');

  return (
    <div
      className={`device-card ${isConnected ? 'device-card--connected' : ''}`}
      onClick={() => navigate(`/device/${device.id}`)}
      role="button"
      tabIndex={0}
      onKeyDown={(e) => e.key === 'Enter' && navigate(`/device/${device.id}`)}
    >
      <div className="device-card__name">{device.displayName}</div>
      <div className="device-card__id">{device.id.slice(0, 12)}</div>
      <div className="device-card__status">
        <span className={`status-dot ${isConnected ? 'status-dot--on' : ''}`} />
        {isConnected ? 'Connected' : 'Available'}
      </div>
      <div className="device-card__transport">{transportLabels}</div>
    </div>
  );
}

export function DeviceList() {
  const { devices } = useDevices();
  const navigate = useNavigate();
  const autoNavigated = useRef(false);

  // Auto-navigate to the only device on first mount (preserves pre-routing
  // single-device UX). The ref prevents an infinite redirect loop when
  // the user presses back from DeviceDetail → DeviceList → auto-nav → DeviceDetail.
  useEffect(() => {
    if (devices.length === 1 && !autoNavigated.current) {
      autoNavigated.current = true;
      navigate(`/device/${devices[0].id}`, { replace: true });
    }
  }, [devices, navigate]);

  const handleAddWebSerial = async () => {
    const source = new WebSerialSource(deviceRegistry);
    try {
      await source.pickAndConnect();
    } catch {
      // User cancelled port picker or connection failed
    }
  };

  return (
    <div className="device-list">
      <header className="device-list__header">
        <h1>Blinky Devices</h1>
        <button className="btn btn-primary" onClick={handleAddWebSerial}>
          Connect USB
        </button>
      </header>

      {devices.length === 0 ? (
        <div className="device-list__empty">
          <p>No devices found.</p>
          <p>Connect a device via USB or wait for server discovery.</p>
        </div>
      ) : (
        <div className="device-list__grid">
          {devices.map((device) => (
            <DeviceCard key={device.id} device={device} />
          ))}
        </div>
      )}
    </div>
  );
}
