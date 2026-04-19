/**
 * Fleet — fleet-level operations applied to all connected devices.
 *
 * Sends commands to all devices via blinky-server's fleet API endpoints.
 * Only available when a BlinkyServerSource is active (same-origin or manual).
 */

import { useState, useCallback } from 'react';
import { useNavigate } from 'react-router-dom';
import { useDevices } from '../hooks/useDevices';
import { logger } from '../lib/logger';

const GENERATORS = ['fire', 'water', 'lightning', 'audio'] as const;
const EFFECTS = ['none', 'hue'] as const;

async function fleetCommand(serverUrl: string, endpoint: string, body?: object): Promise<Record<string, string>> {
  const resp = await fetch(`${serverUrl}${endpoint}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: body ? JSON.stringify(body) : undefined,
    signal: AbortSignal.timeout(10_000),
  });
  if (!resp.ok) throw new Error(`Fleet command failed: ${resp.status}`);
  return resp.json();
}

export function Fleet() {
  const { devices } = useDevices();
  const navigate = useNavigate();
  const [status, setStatus] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);

  const connectedCount = devices.filter(d => d.isConnected()).length;
  // Determine server URL from the first server-sourced transport
  const serverUrl = devices.length > 0
    ? window.location.origin  // Same-origin assumption for now
    : null;

  const runFleetCommand = useCallback(async (label: string, endpoint: string, body?: object) => {
    if (!serverUrl) return;
    setLoading(true);
    setStatus(`Sending: ${label}...`);
    try {
      const results = await fleetCommand(serverUrl, endpoint, body);
      const count = Object.keys(results).length;
      setStatus(`${label}: ${count} device(s) responded`);
      logger.info('Fleet command', { label, results });
    } catch (e) {
      setStatus(`${label}: failed — ${e instanceof Error ? e.message : 'unknown error'}`);
    } finally {
      setLoading(false);
    }
  }, [serverUrl]);

  return (
    <div className="fleet-page">
      <header className="fleet-page__header">
        <button className="btn btn-back" onClick={() => navigate('/')} title="Back to devices">
          &larr;
        </button>
        <h1>Fleet Operations</h1>
        <span className="fleet-page__count">{connectedCount} device(s) connected</span>
      </header>

      {!serverUrl ? (
        <div className="fleet-page__empty">
          <p>No blinky-server detected. Fleet operations require a server connection.</p>
        </div>
      ) : (
        <div className="fleet-page__sections">
          <section className="fleet-section">
            <h2>Generator</h2>
            <div className="fleet-section__buttons">
              {GENERATORS.map(gen => (
                <button
                  key={gen}
                  className="btn"
                  disabled={loading}
                  onClick={() => runFleetCommand(`Set generator: ${gen}`, `/api/fleet/generator/${gen}`)}
                >
                  {gen}
                </button>
              ))}
            </div>
          </section>

          <section className="fleet-section">
            <h2>Effect</h2>
            <div className="fleet-section__buttons">
              {EFFECTS.map(fx => (
                <button
                  key={fx}
                  className="btn"
                  disabled={loading}
                  onClick={() => runFleetCommand(`Set effect: ${fx}`, `/api/fleet/effect/${fx}`)}
                >
                  {fx}
                </button>
              ))}
            </div>
          </section>

          <section className="fleet-section">
            <h2>Settings</h2>
            <div className="fleet-section__buttons">
              <button
                className="btn"
                disabled={loading}
                onClick={() => runFleetCommand('Save settings', '/api/fleet/save')}
              >
                Save All
              </button>
              <button
                className="btn"
                disabled={loading}
                onClick={() => runFleetCommand('Load settings', '/api/fleet/load')}
              >
                Load All
              </button>
              <button
                className="btn btn-danger"
                disabled={loading}
                onClick={() => runFleetCommand('Reset defaults', '/api/fleet/defaults')}
              >
                Reset All to Defaults
              </button>
            </div>
          </section>

          <section className="fleet-section">
            <h2>Custom Command</h2>
            <FleetCommandInput onSend={(cmd) => runFleetCommand(`Command: ${cmd}`, '/api/fleet/command', { command: cmd })} disabled={loading} />
          </section>

          {status && (
            <div className="fleet-status">
              {status}
            </div>
          )}
        </div>
      )}
    </div>
  );
}

function FleetCommandInput({ onSend, disabled }: { onSend: (cmd: string) => void; disabled: boolean }) {
  const [command, setCommand] = useState('');

  const handleSubmit = (e: React.FormEvent) => {
    e.preventDefault();
    if (command.trim()) {
      onSend(command.trim());
      setCommand('');
    }
  };

  return (
    <form className="fleet-command-form" onSubmit={handleSubmit}>
      <input
        type="text"
        value={command}
        onChange={e => setCommand(e.target.value)}
        placeholder="e.g., set bassFluxWeight 0.5"
        disabled={disabled}
        className="fleet-command-input"
      />
      <button type="submit" className="btn btn-primary" disabled={disabled || !command.trim()}>
        Send to All
      </button>
    </form>
  );
}
