/**
 * Fleet — fleet-level operations applied to all connected devices.
 *
 * Sends commands to all devices via blinky-server's fleet API endpoints.
 * Only available when a BlinkyServerSource is active (same-origin or manual).
 */

import { useState, useCallback, useEffect } from 'react';
import { useNavigate } from 'react-router-dom';
import { useDevices } from '../hooks/useDevices';
import { logger } from '../lib/logger';

const GENERATORS = ['fire', 'water', 'lightning', 'audio'] as const;
const EFFECTS = ['none', 'hue'] as const;

interface FirmwareStatus {
  current_version: string | null;
  firmware_path: string | null;
  firmware_available: boolean;
  devices: Array<{
    id: string;
    device_name: string | null;
    version: string | null;
    up_to_date: boolean;
  }>;
  out_of_date_count: number;
}

async function fleetCommand(
  serverUrl: string,
  endpoint: string,
  body?: object
): Promise<Record<string, string>> {
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
  const { devices, serverUrl } = useDevices();
  const navigate = useNavigate();
  const [status, setStatus] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);

  const connectedCount = devices.filter(d => d.isConnected()).length;

  // Firmware status
  const [firmware, setFirmware] = useState<FirmwareStatus | null>(null);
  const [flashJobId, setFlashJobId] = useState<string | null>(null);
  const [flashProgress, setFlashProgress] = useState<string | null>(null);

  const fetchFirmwareStatus = useCallback(async () => {
    if (!serverUrl) return;
    try {
      const resp = await fetch(`${serverUrl}/api/fleet/firmware`, {
        signal: AbortSignal.timeout(5000),
      });
      if (resp.ok) setFirmware(await resp.json());
    } catch {
      /* server unreachable */
    }
  }, [serverUrl]);

  useEffect(() => {
    fetchFirmwareStatus();
  }, [fetchFirmwareStatus]);

  // Poll flash job progress
  useEffect(() => {
    if (!flashJobId || !serverUrl) return;
    const interval = setInterval(async () => {
      try {
        const resp = await fetch(`${serverUrl}/api/fleet/jobs/${flashJobId}`);
        if (!resp.ok) {
          setStatus(`Flash polling failed: HTTP ${resp.status}`);
          setFlashJobId(null);
          setFlashProgress(null);
          return;
        }
        const job = await resp.json();
        setFlashProgress(job.progressMessage || `${job.progress}%`);
        if (job.status === 'complete' || job.status === 'error') {
          setFlashJobId(null);
          setFlashProgress(null);
          setStatus(
            job.status === 'complete'
              ? `Flash complete: ${job.result?.message || 'done'}`
              : `Flash failed: ${job.error || 'unknown'}`
          );
          fetchFirmwareStatus();
        }
      } catch {
        // Transient poll failure — keep trying
      }
    }, 3000);
    return () => clearInterval(interval);
  }, [flashJobId, serverUrl, fetchFirmwareStatus]);

  const handleFlashOutOfDate = useCallback(async () => {
    if (!serverUrl || !firmware?.firmware_path) return;
    setLoading(true);
    setStatus('Submitting flash job...');
    try {
      const resp = await fetch(`${serverUrl}/api/fleet/flash`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ firmware_path: firmware.firmware_path }),
      });
      if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
      const result = await resp.json();
      setFlashJobId(result.job_id);
      setStatus(`Flash submitted: ${result.message}`);
    } catch (e) {
      setStatus(`Flash failed: ${e instanceof Error ? e.message : 'unknown'}`);
    } finally {
      setLoading(false);
    }
  }, [serverUrl, firmware]);

  const runFleetCommand = useCallback(
    async (label: string, endpoint: string, body?: object) => {
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
    },
    [serverUrl]
  );

  return (
    <div className="fleet-page">
      <header className="fleet-page__header">
        <button
          className="btn btn-back"
          onClick={() => navigate('/')}
          title="Back to devices"
          aria-label="Back to devices"
        >
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
          <section className="fleet-section fleet-section--firmware">
            <h2>Firmware</h2>
            {!firmware ? (
              <div className="firmware-status__current">Loading firmware status...</div>
            ) : !firmware.current_version ? (
              <div className="firmware-status">
                <div className="firmware-status__current">
                  No firmware uploaded yet. Use deploy.sh to upload and flash.
                </div>
              </div>
            ) : (
              <div className="firmware-status">
                <div className="firmware-status__version">
                  Server: <strong>{firmware.current_version}</strong>
                </div>
                {firmware.out_of_date_count > 0 && (
                  <div className="firmware-status__outdated">
                    {firmware.out_of_date_count} device(s) need update
                  </div>
                )}
                {firmware.out_of_date_count === 0 && (
                  <div className="firmware-status__current">All devices up to date</div>
                )}
                <div className="firmware-status__devices">
                  {firmware.devices.map(d => (
                    <span
                      key={d.id}
                      className={`firmware-device-badge ${d.up_to_date ? '' : 'firmware-device-badge--outdated'}`}
                    >
                      {d.device_name || d.id.slice(0, 8)}: {d.version || '?'}
                    </span>
                  ))}
                </div>
                {firmware.firmware_available &&
                  firmware.firmware_path &&
                  firmware.out_of_date_count > 0 &&
                  !flashJobId && (
                    <button
                      className="btn btn-primary"
                      disabled={loading}
                      onClick={handleFlashOutOfDate}
                    >
                      Flash Out-of-Date Devices
                    </button>
                  )}
                {flashJobId && (
                  <div className="firmware-status__progress">
                    Flashing: {flashProgress || 'starting...'}
                  </div>
                )}
              </div>
            )}
          </section>

          <section className="fleet-section">
            <h2>Generator</h2>
            <div className="fleet-section__buttons">
              {GENERATORS.map(gen => (
                <button
                  key={gen}
                  className="btn"
                  disabled={loading}
                  onClick={() =>
                    runFleetCommand(`Set generator: ${gen}`, `/api/fleet/generator/${gen}`)
                  }
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
                onClick={() => {
                  if (!confirm('Reset ALL devices to factory defaults?')) return;
                  runFleetCommand('Reset defaults', '/api/fleet/defaults');
                }}
              >
                Reset All to Defaults
              </button>
            </div>
          </section>

          <section className="fleet-section">
            <h2>Custom Command</h2>
            <FleetCommandInput
              onSend={cmd =>
                runFleetCommand(`Command: ${cmd}`, '/api/fleet/command', { command: cmd })
              }
              disabled={loading}
            />
          </section>

          {status && <div className="fleet-status">{status}</div>}
        </div>
      )}
    </div>
  );
}

function FleetCommandInput({
  onSend,
  disabled,
}: {
  onSend: (cmd: string) => void;
  disabled: boolean;
}) {
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
