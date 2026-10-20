/**
 * nRF52840 (XIAO nRF52840 Sense, S140 7.3.0, fleet bootloader 0.8.0-x) flash
 * layout as the web flasher enforces it. Every constant mirrors the host-side
 * validators the fleet tools run; keep them in sync:
 *
 *   APP_START   tools/uf2_upload.py validate_uf2 SD_END_S140_730 (end of the
 *               SoftDevice, where the application starts)
 *   APP_END     tools/uf2_upload.py validate_uf2 docstring: an app UF2 "may
 *               write [0x27000, 0xE6000)". Above it sit InternalFS (/config.bin,
 *               0xED000) and the bootloader (0xF4000), neither of which a
 *               firmware image may touch.
 *
 * The web flasher is stricter than validate_uf2: it refuses MBR/SoftDevice
 * blocks outright (validate_uf2 tolerates them for combined SD+app images,
 * which a release never is).
 */
export const APP_START = 0x27000;
export const APP_END = 0xe6000;

export const UF2_FAMILY_NRF52840_APP = 0xada52840;

/**
 * Bootloaders the recovery path writes to (INFO_UF2.TXT "UF2 Bootloader
 * <ver>"), as deploy_local.py --expect-bl: the fleet bootloader 0.8.0-10 and
 * 0.8.0-11 (UF2 stuck-transfer check armed only by real UF2 blocks).
 */
export const ACCEPTED_BOOTLOADER_PREFIXES = ['0.8.0-10', '0.8.0-11'] as const;
/** INFO_UF2.TXT Board-ID of the XIAO nRF52840 Sense bootloader build. */
export const REQUIRED_BOARD_ID = 'nRF52840-SeeedXiaoSense-v1';
