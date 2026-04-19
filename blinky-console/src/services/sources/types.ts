/**
 * Source-layer types: the device discovery / aggregation layer that sits
 * above {@link DeviceProtocol} and {@link Transport}.
 *
 * A {@link Source} represents an origin from which devices can be
 * reached — WebSerial in the local browser, Web Bluetooth in the local
 * browser, a specific blinky-server over HTTP+WebSocket, etc. Each source
 * implements its own discovery flow (user-gesture pick, passive polling,
 * etc.), then registers the resulting {@link Device} entries with the
 * {@link DeviceRegistry}.
 *
 * A {@link Device} is the canonical handle for one physical device,
 * deduplicated across sources by hardware serial number. It owns the list
 * of transport bindings it can be reached through and the currently-bound
 * {@link DeviceProtocol}.
 */

import type { DeviceProtocol } from '../protocol';
import type { Transport } from '../transport';

export type SourceKind = 'webserial' | 'webbluetooth' | 'blinky-server';

/** Device as reported by blinky-server's GET /api/devices response. */
export interface ServerDevice {
  id: string;
  port: string;
  platform: string;
  transport: string;
  state: string;
  version: string | null;
  device_type: string | null;
  device_name: string | null;
  configured: boolean;
  hardware_sn: string | null;
  ble_address: string | null;
}

export interface Source {
  readonly kind: SourceKind;
  readonly displayName: string;
  isSupported(): boolean;
}

/** A single way a device is reachable: which source delivered it, and the
 *  transport instance bound to it. */
export interface TransportBinding {
  source: Source;
  transport: Transport;
}

/**
 * One physical device, deduplicated across sources.
 *
 * `id` is the canonical identifier — the firmware-reported hardware
 * serial number when known, or a synthesised stub keyed by transport when
 * not (e.g. for very old firmware that doesn't emit `sn`). Two sources
 * reporting the same `id` collapse into the same Device, with their
 * transports merged into a single `transports` list.
 *
 * `protocol` is the currently-bound protocol instance, or `null` when no
 * transport is active. UI in later milestones lets the user pick which
 * transport the protocol is bound to.
 */
export class Device {
  readonly transports: TransportBinding[];
  /** Firmware version reported by the device (e.g., "b130"). Set by sources. */
  public version: string | null = null;

  constructor(
    readonly id: string,
    public displayName: string,
    transports: TransportBinding[],
    public protocol: DeviceProtocol | null = null
  ) {
    this.transports = [...transports];
  }

  /** Add a transport binding, ignoring duplicates by reference. */
  addTransport(binding: TransportBinding): void {
    const isDuplicate = this.transports.some(
      b => b.source === binding.source && b.transport === binding.transport
    );
    if (!isDuplicate) {
      this.transports.push(binding);
    }
  }

  /** Remove a transport binding by transport reference. */
  removeTransport(transport: Transport): void {
    const idx = this.transports.findIndex(b => b.transport === transport);
    if (idx >= 0) this.transports.splice(idx, 1);
  }

  isConnected(): boolean {
    return this.protocol?.isConnected() ?? false;
  }
}
