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

  const selectedDevice = selectedId ? (devices.find(d => d.id === selectedId) ?? null) : null;

  // Lazily initialise the protocol on the picked device, mirroring MainShell.
  // useDeviceAudioStream will then call connect() once the protocol exists.
  useEffect(() => {
    if (!selectedDevice || selectedDevice.protocol || selectedDevice.transports.length === 0) {
      return;
    }
    selectedDevice.protocol = new DeviceProtocol(selectedDevice.transports[0].transport);
  }, [selectedId, selectedDevice]);

  const stream = useDeviceAudioStream(selectedDevice);

  const connectionState = stream.isConnected
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
