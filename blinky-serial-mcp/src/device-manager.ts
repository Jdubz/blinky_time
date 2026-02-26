/**
 * Manages multiple simultaneous device connections.
 * Replaces the global singleton BlinkySerial pattern.
 */

import { SerialPort } from 'serialport';
import { DeviceSession } from './device-session.js';
import type { DeviceInfo } from './types.js';

export class DeviceManager {
  private sessions: Map<string, DeviceSession> = new Map();

  /** List available serial ports (static, no connection needed) */
  async listPorts(): Promise<string[]> {
    const ports = await SerialPort.list();
    return ports.map(p => p.path);
  }

  /** Connect to a device, creating a new session or returning existing */
  async connect(port: string): Promise<{ session: DeviceSession; deviceInfo: DeviceInfo }> {
    const existing = this.sessions.get(port);
    if (existing && existing.getState().connected) {
      const info = existing.getState().deviceInfo;
      if (info) return { session: existing, deviceInfo: info };
      // Session exists but has no deviceInfo — clean up the orphaned session
      await existing.disconnect().catch(() => {});
      this.sessions.delete(port);
    }

    const session = new DeviceSession(port);
    try {
      const deviceInfo = await session.connect();
      this.sessions.set(port, session);
      return { session, deviceInfo };
    } catch (err) {
      // Clean up the session's serial port to prevent file descriptor leak.
      // Without this, a timed-out connect leaves an orphaned DeviceSession
      // with an open port that can never be reached by disconnect().
      await session.disconnect().catch(() => {});
      throw err;
    }
  }

  /** Disconnect a specific device and remove its session */
  async disconnect(port: string): Promise<void> {
    const session = this.sessions.get(port);
    if (session) {
      await session.disconnect();
      this.sessions.delete(port);
    }
  }

  /** Disconnect all devices */
  async disconnectAll(): Promise<void> {
    for (const [, session] of this.sessions) {
      await session.disconnect();
    }
    this.sessions.clear();
  }

  /** Get session by port path */
  getSession(port: string): DeviceSession | undefined {
    this.pruneStale();
    return this.sessions.get(port);
  }

  /** Get all connected sessions */
  getAllSessions(): ReadonlyMap<string, DeviceSession> {
    this.pruneStale();
    return this.sessions;
  }

  /** Get the number of connected devices */
  get connectedCount(): number {
    this.pruneStale();
    return this.sessions.size;
  }

  /** Remove sessions whose serial port has been closed (e.g. device unplugged) */
  private pruneStale(): void {
    for (const [p, s] of this.sessions) {
      if (!s.getState().connected) {
        this.sessions.delete(p);
      }
    }
  }

  /**
   * Resolve which device to target.
   * - If port is specified, return that session (error if not connected).
   * - If port is omitted and exactly one device connected, return it.
   * - If port is omitted and zero or 2+ devices connected, error.
   */
  resolveSession(port?: string): DeviceSession {
    this.pruneStale();

    if (port) {
      const session = this.sessions.get(port);
      if (!session) {
        const connected = [...this.sessions.keys()];
        throw new Error(
          `Not connected to ${port}. Connected devices: ${connected.length > 0 ? connected.join(', ') : 'none'}`
        );
      }
      return session;
    }

    if (this.sessions.size === 0) {
      throw new Error('No devices connected. Use connect(port) first.');
    }

    if (this.sessions.size === 1) {
      return [...this.sessions.values()][0];
    }

    const connected = [...this.sessions.keys()];
    throw new Error(
      `Multiple devices connected (${connected.join(', ')}). Specify a port to target a specific device.`
    );
  }

  /** List info for all connected devices */
  listConnectedDevices(): Array<{ port: string; deviceInfo: DeviceInfo | null; streaming: boolean }> {
    this.pruneStale();
    return [...this.sessions.entries()].map(([port, session]) => {
      const state = session.getState();
      return {
        port,
        deviceInfo: state.deviceInfo,
        streaming: state.streaming,
      };
    });
  }
}
