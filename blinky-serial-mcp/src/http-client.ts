/**
 * HTTP client for blinky-server REST API.
 *
 * All device interaction goes through the server at localhost:8420.
 * WebSocket support for streaming/monitoring tools.
 */

import WebSocket from 'ws';

const BASE_URL = process.env.BLINKY_SERVER_URL || 'http://localhost:8420';

// ---------------------------------------------------------------------------
// REST helpers
// ---------------------------------------------------------------------------

export async function api(method: string, path: string, body?: unknown): Promise<unknown> {
  const url = `${BASE_URL}/api${path}`;
  const options: RequestInit = { method };
  if (body !== undefined) {
    options.headers = { 'Content-Type': 'application/json' };
    options.body = JSON.stringify(body);
  }
  const res = await fetch(url, options);
  const text = await res.text();
  if (!res.ok) {
    throw new Error(`API ${method} ${path}: ${res.status} ${text}`);
  }
  try {
    return JSON.parse(text);
  } catch {
    return text;
  }
}

export const get = (path: string): Promise<unknown> => api('GET', path);
export const post = (path: string, body?: unknown): Promise<unknown> => api('POST', path, body);
export const put = (path: string, body?: unknown): Promise<unknown> => api('PUT', path, body);
export const del = (path: string): Promise<unknown> => api('DELETE', path);

// ---------------------------------------------------------------------------
// Device resolution
// ---------------------------------------------------------------------------

interface DeviceInfo {
  id: string;
  port: string;
  state: string;
  [key: string]: unknown;
}

/**
 * Resolve a port name or device ID to the server's device ID.
 * If no identifier given and only one device connected, returns that device.
 */
export async function resolveDeviceId(portOrId?: string): Promise<string> {
  const devices = (await get('/devices')) as DeviceInfo[];
  const connected = devices.filter((d) => d.state === 'connected');

  if (!portOrId) {
    if (connected.length === 1) return connected[0].id;
    if (connected.length === 0) throw new Error('No connected devices');
    throw new Error(
      `Multiple devices connected (${connected.length}). Specify port or device_id.`,
    );
  }

  // Match by port path
  const byPort = devices.find((d) => d.port === portOrId);
  if (byPort) return byPort.id;

  // Match by ID or prefix
  const byId = devices.find((d) => d.id === portOrId || d.id.startsWith(portOrId));
  if (byId) return byId.id;

  throw new Error(`Device not found: ${portOrId}`);
}

// ---------------------------------------------------------------------------
// WebSocket monitor
// ---------------------------------------------------------------------------

interface WsMessage {
  type: string;
  device_id: string;
  data: Record<string, unknown>;
}

/**
 * Connect to a device's WebSocket stream and collect messages for a duration.
 * Calls handler for each message. Returns when duration elapses or on error.
 */
export async function monitorWs(
  deviceId: string,
  durationMs: number,
  handler: (msg: WsMessage) => void,
): Promise<void> {
  const wsUrl = `${BASE_URL.replace(/^https:/, 'wss:').replace(/^http:/, 'ws:')}/ws/${deviceId}`;
  return new Promise<void>((resolve, reject) => {
    let settled = false;
    const ws = new WebSocket(wsUrl);
    const timer = setTimeout(() => {
      ws.close();
    }, durationMs);
    ws.on('message', (raw: WebSocket.Data) => {
      try {
        const msg = JSON.parse(raw.toString()) as WsMessage;
        handler(msg);
      } catch {
        // Ignore malformed messages
      }
    });
    ws.on('error', (err: Error) => {
      clearTimeout(timer);
      if (!settled) { settled = true; reject(err); }
    });
    ws.on('close', () => {
      clearTimeout(timer);
      if (!settled) { settled = true; resolve(); }
    });
  });
}
