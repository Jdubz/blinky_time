import { logger } from '../../lib/logger';
import { DeviceProtocol } from '../protocol';
import { WebSerialTransport } from '../transport';
import { DeviceRegistry, deviceRegistry } from './DeviceRegistry';
import { Device, Source, SourceKind } from './types';

/**
 * Source for devices reachable via the local browser's WebSerial API.
 *
 * WebSerial requires a user gesture (a click) to invoke `requestPort()`,
 * so this source has no passive discovery — call {@link pickAndConnect}
 * from a click handler. The resulting {@link Device} is registered in the
 * provided registry under its firmware-reported hardware serial number
 * (or a synthesised stub if older firmware doesn't emit one).
 */
export class WebSerialSource implements Source {
  readonly kind: SourceKind = 'webserial';
  readonly displayName = 'USB (this browser)';

  constructor(
    private readonly registry: DeviceRegistry = deviceRegistry,
    private readonly baudRate: number = 115200
  ) {}

  isSupported(): boolean {
    return 'serial' in navigator;
  }

  /**
   * Open the WebSerial port picker, connect, query device info to obtain
   * the canonical hardware serial number, and register the resulting
   * Device. Throws on user cancel or transport failure.
   */
  async pickAndConnect(): Promise<Device> {
    const transport = new WebSerialTransport(this.baudRate);
    await transport.connect(); // throws TransportError on cancel/failure

    const protocol = new DeviceProtocol(transport);
    const info = await protocol.getDeviceInfo();

    const id = info?.sn ?? this.synthesiseLocalId();
    const displayName = info && info.device.configured ? info.device.name : 'Unconfigured Device';

    if (!info?.sn) {
      // Degraded-but-functional path: device works normally (settings, streaming,
      // etc.) but can't be matched to the same device discovered via server or BLE.
      // Reconnecting creates a new registry entry each time. This happens with
      // old firmware that doesn't emit `sn` in `json info`, or on getDeviceInfo timeout.
      logger.warn(
        'WebSerialSource: device did not report sn — using synthesised id. ' +
        'Device is fully functional but cross-source dedup will not work.',
        { id }
      );
    }

    const device = new Device(id, displayName, [{ source: this, transport }], protocol);
    return this.registry.upsert(device);
  }

  /** Stable-enough id for one local-WebSerial connection when the firmware
   *  doesn't expose `sn`. Cross-source dedup will fail for these devices,
   *  and a single device that's reconnected appears as a fresh entry each
   *  time. The monotonic counter prevents same-millisecond collisions. */
  private synthesiseLocalId(): string {
    WebSerialSource.synthCounter += 1;
    return `webserial:local:${Date.now()}:${WebSerialSource.synthCounter}`;
  }

  private static synthCounter = 0;
}
