/**
 * Target — unified dispatch for fleet-wide or single-device operations.
 *
 * A Target is either:
 *   - kind:'fleet'  — commands go through blinky-server's /api/fleet/* endpoints
 *     (reaches every connected device). Requires the same-origin server source.
 *   - kind:'device' — commands go through a single device's DeviceProtocol over
 *     whichever transport it's bound to (WebSerial, server WS, …).
 *
 * The UI holds one Target at a time. Switching the target (e.g. tapping a
 * specific device in the DeviceStrip) just swaps the Target passed into
 * GeneratorSelector / EffectSelector / SettingsPanel — no rewiring.
 */
import type { DeviceProtocol } from '../services/protocol';
import type { GeneratorType, EffectType } from '../types';

export type Target = { kind: 'fleet' } | { kind: 'device'; id: string; protocol: DeviceProtocol };

/**
 * Fleet HTTP client. Always calls same-origin — the console is served
 * from blinky-server (or Caddy-proxied to it), so relative URLs work.
 */
async function fleetPost(path: string): Promise<void> {
  const resp = await fetch(`/api/fleet${path}`, {
    method: 'POST',
    signal: AbortSignal.timeout(10_000),
  });
  if (!resp.ok) throw new Error(`Fleet command failed: ${resp.status} ${path}`);
}

async function fleetPut(path: string, body: unknown): Promise<void> {
  const resp = await fetch(`/api/fleet${path}`, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(10_000),
  });
  if (!resp.ok) throw new Error(`Fleet command failed: ${resp.status} ${path}`);
}

export async function targetSetGenerator(target: Target, name: GeneratorType): Promise<void> {
  if (target.kind === 'device') return target.protocol.setGenerator(name);
  return fleetPost(`/generator/${encodeURIComponent(name)}`);
}

export async function targetSetEffect(target: Target, name: EffectType): Promise<void> {
  if (target.kind === 'device') return target.protocol.setEffect(name);
  return fleetPost(`/effect/${encodeURIComponent(name)}`);
}

export async function targetSetSetting(
  target: Target,
  name: string,
  value: number | boolean
): Promise<void> {
  if (target.kind === 'device') return target.protocol.setSetting(name, value);
  return fleetPut(`/settings/${encodeURIComponent(name)}`, { value });
}
