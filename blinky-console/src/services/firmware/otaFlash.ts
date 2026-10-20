import { WebBluetoothTransport } from '../transport/WebBluetoothTransport';
import { WebSerialTransport } from '../transport/WebSerialTransport';
import type { FlashInfo } from './deviceGates';
import { DeviceLink } from './deviceLink';
import { FirmwareError } from './errors';
import {
  checkAborted,
  finishAfterFlash,
  openUnlessCancelled,
  preflight,
  sleep,
  type FlashUi,
} from './flashFlow';
import { CHUNK, OtaClient, type OtaStatus } from './otaClient';
import type { OtaRelease, ReleaseIndex } from './releases';
import { reconnectApp } from './usbFlash';

/**
 * Update one device through its running app (signed OTA,
 * blinky-things/ota/QspiOtaStaging.h), over USB (Web Serial) or Bluetooth
 * (Web Bluetooth to the app's Nordic UART service). No bootloader drive, no
 * file picker, and the old firmware keeps running until the device has the
 * whole image and has checked it:
 *
 *   connect -> preflight gates -> `ota status` -> signed manifest -> chunks
 *   -> `ota commit` (device verifies, resets; the bootloader installs it)
 *   -> find the device again -> postflight checks (finishAfterFlash)
 *
 * Desktop Chrome/Edge: USB or Bluetooth. Android Chrome: Bluetooth. iPhone:
 * a Web Bluetooth browser such as Bluefy (docs/WEB_FLASHER.md).
 */

export type UpdateMethod = 'usb' | 'ble';

const VID = 0x2886;
const APP_PID = 0x8045;
/** A dropped Bluetooth link mid-transfer is reconnected this many times. */
const BLE_RESUME_ATTEMPTS = 3;

export function bleUpdateSupported(): boolean {
  return typeof navigator !== 'undefined' && 'bluetooth' in navigator;
}

interface Session {
  link: DeviceLink;
  client: OtaClient;
  close(): Promise<void>;
  /** BLE: reconnect to the same device without a picker. */
  reconnect?: () => Promise<void>;
  /** Size bulk BLE writes from the device-reported MTU. */
  tune(status: OtaStatus, ui: FlashUi): void;
}

async function openUsb(ui: FlashUi): Promise<Session> {
  const transport = new WebSerialTransport(115200, [{ usbVendorId: VID, usbProductId: APP_PID }]);
  await ui.ask('Plug the device into this computer with a USB data cable, then choose it.', [
    {
      label: 'Choose USB device',
      action: () =>
        openUnlessCancelled(
          () => transport.connect(),
          () => transport.disconnect(),
          ui
        ),
    },
  ]);
  const link = new DeviceLink(transport);
  return { link, client: new OtaClient(link), close: () => transport.disconnect(), tune: () => {} };
}

async function openBle(ui: FlashUi): Promise<Session> {
  const transport = new WebBluetoothTransport();
  await ui.ask(
    'Turn on Bluetooth, keep this phone or computer next to the device, then choose it.',
    [
      {
        label: 'Choose Bluetooth device',
        action: () =>
          openUnlessCancelled(
            () => transport.connect(),
            () => transport.disconnect(),
            ui
          ),
      },
    ]
  );
  const link = new DeviceLink(transport);
  const client = new OtaClient(link, {
    enable: chunk => transport.bulkWrites(chunk),
    disable: () => transport.defaultWrites(),
  });
  return {
    link,
    client,
    close: () => transport.disconnect(),
    reconnect: async () => {
      await Promise.race([
        transport.reconnect(),
        sleep(15_000).then(() => {
          throw new FirmwareError('Bluetooth reconnect timed out');
        }),
      ]);
    },
    tune: (status, u) => {
      // ATT payload = MTU - 3. Below that, keep the 20-byte acknowledged writes.
      const chunk = Math.min(status.mtu - 3, 244);
      if (chunk > 20) {
        transport.bulkWrites(chunk);
        u.log(`Bluetooth link MTU ${status.mtu}: ${chunk}-byte writes`);
      } else {
        u.log(`Bluetooth link MTU ${status.mtu}: small writes (slower)`, 'warn');
      }
    },
  };
}

/** Find the device again after the bootloader installed the image. */
async function reopenAfterApply(
  method: UpdateMethod,
  session: Session,
  sn: string,
  ui: FlashUi
): Promise<{ link: DeviceLink; close(): Promise<void> }> {
  if (method === 'usb') {
    await session.close();
    const back = await reconnectApp(sn, ui);
    return { link: back.link, close: () => back.transport.disconnect() };
  }
  ui.progress('Waiting for the device to restart on the new firmware', null);
  // The bootloader copies ~465 KB into internal flash (~10-15 s) before the
  // app boots and advertises again.
  for (let i = 0; i < 8; i++) {
    await sleep(5000);
    try {
      await session.reconnect!();
      const info = await session.link.info();
      if (info.sn.toUpperCase() !== sn.toUpperCase()) {
        throw new FirmwareError(`A different device answered (sn ${info.sn}, expected ${sn})`);
      }
      return { link: session.link, close: session.close };
    } catch (err) {
      ui.log(`Not back yet (${err instanceof Error ? err.message : String(err)})`, 'warn');
    }
  }
  throw new FirmwareError(
    "The device didn't reappear over Bluetooth after installing. Plug it in by USB and use this page's USB " +
      'option to check it; do not unplug its power while its light shows activity.'
  );
}

export async function updateDevice(
  method: UpdateMethod,
  release: OtaRelease,
  index: ReleaseIndex,
  ui: FlashUi
): Promise<FlashInfo> {
  const session = method === 'usb' ? await openUsb(ui) : await openBle(ui);
  let closed = false;
  try {
    const before = await preflight(session.link, release, index, ui);
    let status = await session.client.status();
    if (status.maxChunk < CHUNK) {
      throw new FirmwareError(
        `The device accepts ${status.maxChunk}-byte chunks; this page sends ${CHUNK}`
      );
    }
    ui.log(`Device update support: protocol 1, ${status.key} key, running build ${status.current}`);
    session.tune(status, ui);

    let from = 0;
    if (
      status.state === 'receiving' &&
      status.build === release.fields.build &&
      status.size === release.fields.size
    ) {
      from = status.next;
      ui.log(`Resuming an interrupted transfer at ${from} of ${status.size} bytes`);
    } else {
      checkAborted(ui);
      ui.progress('Checking the update on the device', null);
      await session.client.begin(release);
      ui.log(`The device accepted the signed update for build ${release.fields.build}`);
    }

    // Transfer; a dropped Bluetooth link is reconnected and resumed from
    // the device's own count. The running firmware is untouched throughout.
    for (let attempt = 0; ; attempt++) {
      try {
        await session.client.transfer(release, from, {
          signal: ui.signal,
          log: ui.log,
          onProgress: (sent, total) =>
            ui.progress(`Sending the update (${Math.round((sent / total) * 100)}%)`, sent / total),
        });
        break;
      } catch (err) {
        const dropped = err instanceof FirmwareError && /disconnected|No reply/.test(err.message);
        if (!dropped || !session.reconnect || attempt >= BLE_RESUME_ATTEMPTS || ui.signal.aborted)
          throw err;
        ui.log(`Connection lost (${err.message}); reconnecting…`, 'warn');
        await session.reconnect();
        status = await session.client.status();
        session.tune(status, ui);
        if (status.state !== 'receiving' || status.build !== release.fields.build) {
          // The device restarted meanwhile: nothing was installed; start over.
          await session.client.begin(release);
          from = 0;
        } else {
          from = status.next;
        }
        ui.log(`Resuming at ${from} bytes`);
      }
    }

    checkAborted(ui);
    ui.committed();
    ui.progress('Installing (the device checks the update, then restarts)', null);
    await session.client.commit();
    ui.log('The device verified the update and is installing it');
    closed = true;
    const back = await reopenAfterApply(method, session, before.sn, ui);
    try {
      return await finishAfterFlash(back.link, before, release.entry.version, ui);
    } finally {
      await back.close();
    }
  } finally {
    if (!closed) await session.close();
  }
}
