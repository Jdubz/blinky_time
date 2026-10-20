import { WebBluetoothTransport } from '../transport/WebBluetoothTransport';
import type { FlashInfo } from './deviceGates';
import { DeviceLink } from './deviceLink';
import { FirmwareError } from './errors';
import { checkAborted, finishAfterFlash, preflight, sleep, type FlashUi } from './flashFlow';
import {
  connectWebBluetoothDfu,
  DFU_DEVICE_NAME,
  DFU_SERVICE,
  DfuTransferError,
  transferLegacyDfu,
} from './legacyDfu';
import type { ReleaseIndex, VerifiedRelease } from './releases';

/**
 * Bluetooth update of one device: the browser version of the fleet's BLE DFU
 * (blinky_server/firmware/ble_dfu.py _ble_dfu_write_impl). Android Chrome and
 * desktop Chrome / Edge; not iOS.
 *
 * Unlike USB, legacy DFU erases the app before the transfer. A transfer that
 * fails after that leaves the device in its bootloader (advertising AdaDFU)
 * with no app; it stays there, and the recovery flow here (or USB) finishes
 * the job. The bootloader's USB path is always available as well.
 */

/** ble_dfu.py MAX_DFU_ATTEMPTS */
const MAX_ATTEMPTS = 3;

export function bleFlashSupported(): boolean {
  return typeof navigator !== 'undefined' && 'bluetooth' in navigator;
}

async function pickBootloader(): Promise<BluetoothDevice> {
  try {
    return await navigator.bluetooth.requestDevice({
      filters: [{ name: DFU_DEVICE_NAME }],
      optionalServices: [DFU_SERVICE],
    });
  } catch (err) {
    throw new FirmwareError(`No bootloader chosen (${String(err)})`);
  }
}

async function dfuWithRetries(
  bootloader: BluetoothDevice,
  release: VerifiedRelease,
  ui: FlashUi
): Promise<void> {
  const total = release.dfu.firmware.length;
  for (let attempt = 1; ; attempt++) {
    try {
      ui.progress(`Connecting to the bootloader (attempt ${attempt}/${MAX_ATTEMPTS})`, null);
      const gatt = await connectWebBluetoothDfu(bootloader);
      await transferLegacyDfu(gatt, release.dfu, p => {
        if (p.phase === 'start') {
          ui.committed();
          ui.progress('Erasing the old firmware', null);
        } else if (p.phase === 'transfer') {
          ui.progress(`Sending firmware (${Math.round((p.sent / total) * 100)}%)`, p.sent / total);
        } else {
          ui.progress(p.phase === 'validate' ? 'Validating' : 'Restarting the device', null);
        }
      });
      return;
    } catch (err) {
      const e = err instanceof DfuTransferError ? err : new DfuTransferError(String(err), false);
      ui.log(`Attempt ${attempt} failed: ${e.message}`, 'error');
      if (attempt >= MAX_ATTEMPTS) {
        throw new FirmwareError(
          e.eraseStarted
            ? 'The update could not be completed. The device is waiting in its bootloader (it may look ' +
                'dark) and is safe. Keep it powered, reload this page and choose "Device stuck in update ' +
                'mode", or connect it by USB.'
            : `The update could not start: ${e.message}. The device's firmware was not changed.`
        );
      }
      // Let the bootloader finish resetting and re-advertise (ble_dfu.py).
      await sleep(e.invalidState ? 15_000 : 20_000);
    }
  }
}

/** Reconnect to the app after DFU. The same BluetoothDevice reconnects without a picker. */
async function reconnectApp(
  transport: WebBluetoothTransport,
  expectSn: string | null,
  ui: FlashUi
): Promise<DeviceLink> {
  ui.progress('Waiting for the device to restart on the new firmware', null);
  const check = async () => {
    const link = new DeviceLink(transport);
    const info = await link.info();
    if (expectSn && info.sn.toUpperCase() !== expectSn.toUpperCase()) {
      await transport.disconnect();
      throw new FirmwareError(`A different device answered (sn ${info.sn}, expected ${expectSn})`);
    }
    return link;
  };
  if (transport.getDevice()) {
    for (let i = 0; i < 6; i++) {
      await sleep(5000);
      try {
        // Chrome's gatt.connect() to a device that is still rebooting can hang.
        await Promise.race([
          transport.reconnect(),
          sleep(15_000).then(() => {
            throw new FirmwareError('connect timed out');
          }),
        ]);
        return await check();
      } catch (err) {
        ui.log(`Not back yet (${err instanceof Error ? err.message : String(err)})`, 'warn');
      }
    }
  }
  return ui.ask('Choose your device again so the page can check the update.', [
    {
      label: 'Choose Bluetooth device',
      action: async () => {
        await transport.connect();
        return check();
      },
    },
  ]);
}

export async function flashOverBle(
  release: VerifiedRelease,
  index: ReleaseIndex,
  ui: FlashUi
): Promise<FlashInfo> {
  const transport = new WebBluetoothTransport();
  await ui.ask(
    'Turn on Bluetooth, keep this phone or computer next to the device, then choose it.',
    [{ label: 'Choose Bluetooth device', action: () => transport.connect() }]
  );
  const link = new DeviceLink(transport);
  let before: FlashInfo;
  try {
    before = await preflight(link, release, index, ui);
    checkAborted(ui);
  } catch (err) {
    await transport.disconnect();
    throw err;
  }
  // The app drops the link as it resets into the bootloader; the transport
  // keeps the BluetoothDevice so it can reconnect to the app afterwards.
  ui.log('Restarting the device into its Bluetooth update mode');
  await link.fire('bootloader ble');
  await sleep(3000);

  const bootloader = await ui.ask(
    `In the next list choose "${DFU_DEVICE_NAME}" — your device in update mode. If several appear, ` +
      'pick the one with the strongest signal and keep this phone next to your device.',
    [{ label: `Choose ${DFU_DEVICE_NAME}`, action: pickBootloader }]
  );
  await dfuWithRetries(bootloader, release, ui);
  const back = await reconnectApp(transport, before.sn, ui);
  try {
    return await finishAfterFlash(back, before, release.entry.version, ui);
  } finally {
    await transport.disconnect();
  }
}

/** A device already in its bootloader (advertising AdaDFU, e.g. after an interrupted update). */
export async function recoverOverBle(release: VerifiedRelease, ui: FlashUi): Promise<FlashInfo> {
  const bootloader = await ui.ask(
    `Keep this phone next to the device. In the next list choose "${DFU_DEVICE_NAME}".`,
    [{ label: `Choose ${DFU_DEVICE_NAME}`, action: pickBootloader }]
  );
  await dfuWithRetries(bootloader, release, ui);
  const transport = new WebBluetoothTransport();
  const link = await ui.ask(
    'The device is restarting. Choose it (its name starts with "Blinky").',
    [
      {
        label: 'Choose Bluetooth device',
        action: async () => {
          await transport.connect();
          return new DeviceLink(transport);
        },
      },
    ]
  );
  try {
    return await finishAfterFlash(link, null, release.entry.version, ui);
  } finally {
    await transport.disconnect();
  }
}
