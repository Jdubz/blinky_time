import { WebSerialTransport } from '../transport/WebSerialTransport';
import type { FlashInfo } from './deviceGates';
import { DeviceLink } from './deviceLink';
import { FirmwareError } from './errors';
import { checkAborted, finishAfterFlash, preflight, sleep, type FlashUi } from './flashFlow';
import { REQUIRED_BOARD_ID, REQUIRED_BOOTLOADER_PREFIX } from './flashLayout';
import type { ReleaseIndex, VerifiedRelease } from './releases';

/**
 * USB update of one device, the browser version of tools/deploy_local.py:
 * Web Serial for the gates and the bootloader entry, then the UF2 written to
 * the bootloader's XIAO-SENSE drive (File System Access API, or copied by
 * hand), then Web Serial again to verify. Chrome / Edge on desktop only.
 *
 * Identity: a browser can't read the drive's USB serial, so the drive can't
 * be pinned to the chip before writing as deploy_local.py does. Instead the
 * drive's INFO_UF2.TXT must be the fleet bootloader on a XIAO nRF52840 Sense,
 * and the device that comes back must be the same chip (sn) on the new
 * version. Writing to another XIAO would install the same stable release on
 * it and fail verification loudly.
 */

const VID = 0x2886;
const APP_PID = 0x8045;
const UF2_NAME = 'blinky-things.uf2';
/** deploy_local.py WRITE_TIMEOUT_S + REENUM_TIMEOUT_S */
const RETURN_TIMEOUT_MS = 135_000;

export function usbFlashSupported(): boolean {
  return typeof navigator !== 'undefined' && 'serial' in navigator;
}

export function drivePickerSupported(): boolean {
  return typeof window !== 'undefined' && typeof window.showDirectoryPicker === 'function';
}

/** INFO_UF2.TXT → bootloader version and board id (ghostfat.c infoUf2File). */
export function parseInfoUf2(text: string): { bootloader: string | null; boardId: string | null } {
  const bl = /^UF2 Bootloader (\S+)/m.exec(text);
  const board = /^Board-ID: (\S+)/m.exec(text);
  return { bootloader: bl?.[1] ?? null, boardId: board?.[1] ?? null };
}

/**
 * Refuse anything but the fleet bootloader on a XIAO nRF52840 Sense. Runs
 * before a UF2 is written or offered for manual copying.
 */
function checkInfoUf2(text: string): void {
  const { bootloader, boardId } = parseInfoUf2(text);
  if (boardId !== REQUIRED_BOARD_ID) {
    throw new FirmwareError(
      `That drive is a "${boardId ?? 'unknown'}" board, not a blinky device (${REQUIRED_BOARD_ID}).`
    );
  }
  if (!bootloader?.startsWith(REQUIRED_BOOTLOADER_PREFIX)) {
    throw new FirmwareError(
      `The device's bootloader is ${bootloader ?? 'unknown'}; this page only updates devices on ` +
        `${REQUIRED_BOOTLOADER_PREFIX}. Nothing was written — press the reset button once to return to the old firmware.`
    );
  }
}

/** Let the user pick INFO_UF2.TXT from the drive with a plain file input (every browser has one). */
function pickInfoUf2File(): Promise<string> {
  return new Promise((resolve, reject) => {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.txt,.TXT';
    input.addEventListener('change', () => {
      const file = input.files?.[0];
      if (!file) return reject(new FirmwareError('No file chosen'));
      if (file.name.toUpperCase() !== 'INFO_UF2.TXT') {
        return reject(
          new FirmwareError(`Choose INFO_UF2.TXT on the XIAO-SENSE drive, not ${file.name}`)
        );
      }
      file.text().then(resolve, reject);
    });
    input.addEventListener('cancel', () => reject(new FirmwareError('No file chosen')));
    input.click();
  });
}

async function pickBootloaderDrive(): Promise<FileSystemDirectoryHandle> {
  let dir: FileSystemDirectoryHandle;
  try {
    dir = await window.showDirectoryPicker!({ id: 'blinky-uf2', mode: 'readwrite' });
  } catch (err) {
    if (err instanceof DOMException && err.name === 'AbortError')
      throw new FirmwareError('No drive chosen');
    throw new FirmwareError(`The browser would not open that drive: ${String(err)}`);
  }
  let text: string;
  try {
    text = await (await (await dir.getFileHandle('INFO_UF2.TXT')).getFile()).text();
  } catch {
    throw new FirmwareError(
      `"${dir.name}" is not a XIAO bootloader drive (no INFO_UF2.TXT). Choose XIAO-SENSE.`
    );
  }
  checkInfoUf2(text);
  return dir;
}

async function writeUf2(
  dir: FileSystemDirectoryHandle,
  data: Uint8Array<ArrayBuffer>,
  ui: FlashUi
): Promise<void> {
  const file = await dir.getFileHandle(UF2_NAME, { create: true });
  const out = await file.createWritable();
  try {
    await out.write(data);
    await out.close();
  } catch (err) {
    // A refused write never reached the device: nothing was written.
    if (
      err instanceof DOMException &&
      (err.name === 'NotAllowedError' || err.name === 'SecurityError')
    ) {
      throw new FirmwareError(`The browser refused to write to the drive: ${err.message}`);
    }
    // The bootloader resets as soon as the last block lands, which can tear
    // the drive down before the browser finishes writing or closing. Only the
    // version check after the device comes back says whether it took
    // (deploy_local.py does the same).
    ui.log(`The drive went away during the write (${String(err)}); checking the device…`, 'warn');
  }
}

function downloadUf2(release: VerifiedRelease): void {
  const url = URL.createObjectURL(new Blob([release.uf2], { type: 'application/octet-stream' }));
  const a = document.createElement('a');
  a.href = url;
  a.download = `blinky-things-${release.entry.tag}.uf2`;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 60_000);
}

/** Exported for tests. */
export async function deliverUf2(release: VerifiedRelease, ui: FlashUi): Promise<void> {
  type Choice = 'written' | 'manual';
  for (;;) {
    const options: { label: string; action: () => Promise<Choice> }[] = [];
    if (drivePickerSupported()) {
      options.push({
        label: 'Choose the XIAO-SENSE drive',
        action: async () => {
          const dir = await pickBootloaderDrive();
          // No await between this check and the write starting.
          checkAborted(ui);
          ui.committed();
          ui.progress('Writing the firmware', null);
          await writeUf2(dir, release.uf2, ui);
          return 'written';
        },
      });
    }
    options.push({
      label: 'Copy the file myself',
      action: async () => {
        // Same bootloader gate as the drive path, before the file is handed out.
        checkInfoUf2(await pickInfoUf2File());
        checkAborted(ui);
        ui.committed();
        return 'manual';
      },
    });
    let choice: Choice;
    try {
      choice = await ui.ask(
        'A drive named XIAO-SENSE should appear (the device LED pulses). Choose it so the page can ' +
          'write the update — or copy the file yourself (the page first asks for INFO_UF2.TXT from ' +
          'that drive, to check the bootloader).',
        options
      );
    } catch (err) {
      if (!(err instanceof FirmwareError) || err.message === 'Cancelled') throw err;
      ui.log(err.message, 'error');
      continue;
    }
    if (choice === 'written') return;
    await ui.ask(
      `Save blinky-things-${release.entry.tag}.uf2, then drag it onto the XIAO-SENSE drive. The drive ` +
        'disappears when the device has taken it.',
      [
        {
          label: `Download blinky-things-${release.entry.tag}.uf2`,
          action: async () => downloadUf2(release),
        },
      ]
    );
    await ui.ask('When the XIAO-SENSE drive has disappeared:', [
      { label: "I've copied it", action: async () => {} },
    ]);
    return;
  }
}

/** Find the device on USB again after the bootloader hands back to the app. Exported for tests. */
export async function reconnectApp(
  expectSn: string | null,
  ui: FlashUi
): Promise<{ transport: WebSerialTransport; link: DeviceLink }> {
  const tryPort = async (port?: SerialPort) => {
    const transport = new WebSerialTransport(115200, [{ usbVendorId: VID, usbProductId: APP_PID }]);
    await transport.connect(port);
    const link = new DeviceLink(transport);
    try {
      const info = await link.info();
      if (expectSn && info.sn.toUpperCase() !== expectSn.toUpperCase()) {
        throw new FirmwareError(`that is a different device (sn ${info.sn}, expected ${expectSn})`);
      }
      return { transport, link };
    } catch (err) {
      await transport.disconnect();
      throw err;
    }
  };

  // Without an expected serial (recovery) a permitted port can't be told
  // apart from another XIAO on this computer: the user must choose it.
  if (!expectSn) {
    return ui.ask('When the device has restarted, choose it so the page can check the update.', [
      { label: 'Choose USB device', action: () => tryPort() },
    ]);
  }
  ui.progress('Waiting for the device to restart on the new firmware', null);
  const deadline = Date.now() + RETURN_TIMEOUT_MS;
  while (Date.now() < deadline) {
    await sleep(3000);
    const ports = (await navigator.serial.getPorts()).filter(p => {
      const i = p.getInfo();
      return i.usbVendorId === VID && i.usbProductId === APP_PID;
    });
    for (const port of ports) {
      try {
        return await tryPort(port);
      } catch (err) {
        // Not up yet (expected while it boots), or another permitted XIAO:
        // keep looking until the deadline. Say so if it's another device.
        if (err instanceof FirmwareError && err.message.includes('different device'))
          ui.log(err.message, 'warn');
      }
    }
  }
  // The browser may not re-grant the re-enumerated port: ask once, by hand.
  return ui.ask('The device has restarted. Choose it again so the page can check the update.', [
    { label: 'Choose USB device', action: () => tryPort() },
  ]);
}

export async function flashOverUsb(
  release: VerifiedRelease,
  index: ReleaseIndex,
  ui: FlashUi
): Promise<FlashInfo> {
  const transport = new WebSerialTransport(115200, [{ usbVendorId: VID, usbProductId: APP_PID }]);
  await ui.ask('Plug the device into this computer with a USB data cable, then choose it.', [
    { label: 'Choose USB device', action: () => transport.connect() },
  ]);
  let before: FlashInfo;
  try {
    before = await preflight(new DeviceLink(transport), release, index, ui);
    checkAborted(ui);
    ui.log(
      'Restarting the device into its bootloader (its current firmware stays in place until the write)'
    );
    await new DeviceLink(transport).fire('bootloader');
  } finally {
    await transport.disconnect();
  }
  await deliverUf2(release, ui);
  const back = await reconnectApp(before.sn, ui);
  try {
    return await finishAfterFlash(back.link, before, release.entry.version, ui);
  } finally {
    await back.transport.disconnect();
  }
}

/** A device already showing its XIAO-SENSE drive (deploy_local.py --already-in-bootloader). */
export async function recoverOverUsb(release: VerifiedRelease, ui: FlashUi): Promise<FlashInfo> {
  await deliverUf2(release, ui);
  const back = await reconnectApp(null, ui);
  try {
    return await finishAfterFlash(back.link, null, release.entry.version, ui);
  } finally {
    await back.transport.disconnect();
  }
}
