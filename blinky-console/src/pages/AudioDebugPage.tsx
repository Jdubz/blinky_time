/**
 * AudioDebugPage — single-device audio stream preview.
 *
 * Debug-only view: pick one device, open its DeviceProtocol, and render the
 * AudioVisualizer chart with onset overlays. Independent from the fleet
 * controls in MainShell — uses its own per-device subscription.
 */

import { useEffect, useState } from 'react';
import { AudioVisualizer } from '../components/AudioVisualizer';
import { useDeviceAudioStream } from '../hooks/useDeviceAudioStream';
import { DeviceProtocol } from '../services/protocol';
import type { Device } from '../services/sources';

interface AudioDebugPageProps {
  devices: Device[];
  onClose: () => void;
}

export function AudioDebugPage({ devices, onClose }: AudioDebugPageProps) {
  const [selectedId, setSelectedId] = useState<string | null>(null);
  // `protocolNonce` bumps after we lazily attach a protocol to a device.
  // Mutating `selectedDevice.protocol` doesn't change object identity, so
  // without this bump useDeviceAudioStream wouldn't re-render to see the
  // new protocol. The hook also keys on `device.protocol` internally, but
  // that re-run only happens if THIS component re-renders first.
  const [, setProtocolNonce] = useState(0);

  const selectedDevice = selectedId ? (devices.find(d => d.id === selectedId) ?? null) : null;

  // Lazily initialise the protocol on the picked device, mirroring MainShell.
  // First-writer-wins: if MainShell or another view already attached a
  // protocol to this Device object, we leave it alone. This is safe because
  // Device objects are shared by reference across the whole app.
  useEffect(() => {
    if (!selectedDevice || selectedDevice.protocol || selectedDevice.transports.length === 0) {
      return;
    }
    selectedDevice.protocol = new DeviceProtocol(selectedDevice.transports[0].transport);
    setProtocolNonce(n => n + 1);
  }, [selectedId, selectedDevice]);

  const stream = useDeviceAudioStream(selectedDevice);

  const connectionState = stream.error
    ? 'error'
    : stream.isConnected
      ? 'connected'
      : selectedDevice
        ? 'connecting'
        : 'disconnected';

  return (
    <div className="audio-debug-page">
      <div className="audio-debug-header">
        <button className="btn btn-small" onClick={onClose}>
          ← Back
        </button>
        <h2 className="audio-debug-title">Audio Stream Debug</h2>
        <select
          className="audio-debug-device-picker"
          value={selectedId ?? ''}
          onChange={e => setSelectedId(e.target.value || null)}
        >
          <option value="">Select a device…</option>
          {devices.map(d => (
            <option key={d.id} value={d.id}>
              {d.displayName}
              {d.transports.length > 0 ? ` · ${d.transports[0].source.kind}` : ''}
            </option>
          ))}
        </select>
        <span className={`audio-debug-status audio-debug-status-${connectionState}`}>
          {connectionState}
        </span>
      </div>

      {stream.error && selectedDevice && (
        <div className="audio-debug-placeholder">
          {`Connection failed: ${stream.error.message}. Check the device is online and try selecting it again.`}
        </div>
      )}

      {!selectedDevice && (
        <div className="audio-debug-placeholder">
          Pick a device above to start previewing its audio stream.
        </div>
      )}

      {selectedDevice && (
        <AudioVisualizer
          audioData={stream.audioData}
          musicModeData={stream.musicModeData}
          isStreaming={stream.isStreaming}
          onToggleStreaming={() => {
            stream.toggleStreaming().catch(err => {
              console.error('Failed to toggle audio stream', err);
            });
          }}
          disabled={!stream.isConnected}
          onTransientEvent={stream.onTransientEvent}
          connectionState={connectionState}
        />
      )}
    </div>
  );
}
