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
import type { Device } from '../services/sources';
import type { DeviceProtocol } from '../services/protocol';

interface AudioDebugPageProps {
  devices: Device[];
  onClose: () => void;
}

export function AudioDebugPage({ devices, onClose }: AudioDebugPageProps) {
  const [selectedId, setSelectedId] = useState<string | null>(null);
  // Tracking the current protocol in component state (rather than the prior
  // nonce-bump-after-mutating-Device pattern) means the hook's deps see a
  // real React-tracked value change when we attach a protocol — no implicit
  // dependency on Device.protocol mutation order. The mutation still
  // happens inside Device.ensureProtocol() so MainShell and this page see
  // the same protocol instance for a given Device, but only this component
  // tracks it for its own rendering.
  const [protocol, setProtocol] = useState<DeviceProtocol | null>(null);

  const selectedDevice = selectedId ? (devices.find(d => d.id === selectedId) ?? null) : null;

  useEffect(() => {
    if (!selectedDevice) {
      setProtocol(null);
      return;
    }
    // ensureProtocol is idempotent: returns the existing Device.protocol if
    // MainShell or another view already attached one, otherwise creates and
    // stores a fresh one. The "first writer wins" invariant is encoded in
    // the method, not duplicated at every call site.
    setProtocol(selectedDevice.ensureProtocol());
    // selectedDevice is derived from selectedId via devices.find(); keying
    // on selectedId alone is correct — adding selectedDevice would re-run
    // the effect on identity changes without triggering meaningful work.
  }, [selectedId]); // eslint-disable-line react-hooks/exhaustive-deps

  const stream = useDeviceAudioStream(selectedDevice, protocol);

  // Decouple machine state (used for CSS class + tests) from display label
  // (user-facing copy) so a rename of one doesn't silently break the other.
  const connectionState: 'error' | 'connected' | 'connecting' | 'disconnected' = stream.error
    ? 'error'
    : stream.isConnected
      ? 'connected'
      : selectedDevice
        ? 'connecting'
        : 'disconnected';
  const connectionLabel: Record<typeof connectionState, string> = {
    error: 'error',
    connected: 'connected',
    connecting: 'connecting',
    disconnected: 'disconnected',
  };

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
        <span
          className={`audio-debug-status audio-debug-status-${connectionState}`}
          role="status"
          aria-live="polite"
        >
          {connectionLabel[connectionState]}
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
