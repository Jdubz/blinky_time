import { Device } from './types';

export type DeviceListListener = (devices: Device[]) => void;

/**
 * In-memory registry of all known {@link Device} entries across every
 * active {@link Source}. Devices are keyed by {@link Device.id} (the
 * canonical hardware serial number when known). Sources call
 * {@link upsert} to register or merge devices; the UI subscribes for
 * change notifications.
 *
 * The registry intentionally has no opinion on connection lifecycle — it
 * tracks identity, not state. Sources own their transports and protocols.
 */
export class DeviceRegistry {
  private devices = new Map<string, Device>();
  private listeners: DeviceListListener[] = [];

  /** Snapshot of all registered devices in insertion order. */
  list(): Device[] {
    return Array.from(this.devices.values());
  }

  get(id: string): Device | undefined {
    return this.devices.get(id);
  }

  has(id: string): boolean {
    return this.devices.has(id);
  }

  /**
   * Register a device, or merge into an existing entry with the same id.
   *
   * If the id already exists, the incoming device's transport bindings are
   * appended (skipping duplicates) and its protocol is bound to the
   * existing entry only if the existing entry has none. Display name is
   * updated only if the existing entry hasn't been given one (length 0).
   *
   * Returns the canonical {@link Device} entry — callers should always use
   * the returned reference, never the one they passed in.
   */
  upsert(device: Device): Device {
    const existing = this.devices.get(device.id);
    if (existing) {
      device.transports.forEach(b => existing.addTransport(b));
      if (!existing.displayName && device.displayName) {
        existing.displayName = device.displayName;
      }
      if (!existing.protocol && device.protocol) {
        existing.protocol = device.protocol;
      }
      this.notify();
      return existing;
    }
    this.devices.set(device.id, device);
    this.notify();
    return device;
  }

  /** Remove a device by id. Caller is responsible for tearing down any
   *  active protocol/transports first. */
  remove(id: string): boolean {
    const removed = this.devices.delete(id);
    if (removed) this.notify();
    return removed;
  }

  /** Clear all entries. Caller is responsible for tearing down protocols. */
  clear(): void {
    if (this.devices.size > 0) {
      this.devices.clear();
      this.notify();
    }
  }

  /** Subscribe to list changes. Returns an unsubscribe function. */
  subscribe(callback: DeviceListListener): () => void {
    this.listeners.push(callback);
    return () => {
      this.listeners = this.listeners.filter(l => l !== callback);
    };
  }

  private notify(): void {
    const list = this.list();
    this.listeners.forEach(cb => cb(list));
  }
}

/**
 * Process-wide registry singleton. Sources push devices here as they
 * discover them; UI components read and subscribe for live updates.
 *
 * UI doesn't yet consume this in M7 — it's foundational for the
 * multi-device list view in M11+.
 */
export const deviceRegistry = new DeviceRegistry();
