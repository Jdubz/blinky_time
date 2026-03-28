#pragma once
/**
 * Uf2BootloaderOverride.h - Fix 1200-baud touch to enter UF2 mode
 *
 * The Seeed nRF52 Arduino core's TinyUSB CDC callback (tud_cdc_line_state_cb)
 * calls TinyUSB_Port_EnterDFU() which calls enterSerialDfu(), setting
 * GPREGRET=0x4E (serial DFU mode). This is wrong for headless operation --
 * we need UF2 mass storage mode (GPREGRET=0x57) so we can copy firmware
 * via a simple file copy to the UF2 drive.
 *
 * This file overrides the weak tud_cdc_line_state_cb symbol from the
 * TinyUSB library. Our version uses the SoftDevice API (same as
 * SerialConsole's "bootloader" command) to reliably set GPREGRET=0x57.
 *
 * The library's definition lives in Adafruit_USBD_CDC.cpp which is compiled
 * into the core.a archive. Since sketch object files are linked before
 * archives, our strong definition takes precedence at link time.
 */

#ifdef ARDUINO_ARCH_NRF52

#include <Arduino.h>

// TinyUSB CDC device API (provides tud_cdc_get_line_coding, cdc_line_coding_t)
#include <class/cdc/cdc_device.h>

// nRF52 SoftDevice API (for reliable GPREGRET writes)
extern "C" {
  #include <nrf_sdm.h>
  #include <nrf_soc.h>
}

extern "C" {

/**
 * Override TinyUSB CDC line state callback.
 *
 * When the host disconnects CDC at 1200 baud (the "touch 1200" protocol),
 * we set GPREGRET=0x57 to enter UF2 mass storage bootloader mode instead
 * of the default serial DFU mode (0x4E).
 *
 * Uses the SoftDevice API when SoftDevice is enabled, matching the
 * pattern from SerialConsole.cpp's "bootloader" command.
 */
void tud_cdc_line_state_cb(uint8_t instance, bool dtr, bool rts) {
    (void)rts;

    // DTR = false is counted as disconnected
    if (!dtr) {
        // touch1200 only with first CDC instance (Serial)
        if (instance == 0) {
            cdc_line_coding_t coding;
            tud_cdc_get_line_coding(&coding);

            if (coding.bit_rate == 1200) {
                // Enter UF2 mass storage bootloader (NOT serial DFU)
                const uint8_t DFU_MAGIC_UF2 = 0x57;
                uint8_t sd_en = 0;
                sd_softdevice_is_enabled(&sd_en);
                if (sd_en) {
                    sd_power_gpregret_clr(0, 0xFF);
                    sd_power_gpregret_set(0, DFU_MAGIC_UF2);
                } else {
                    NRF_POWER->GPREGRET = DFU_MAGIC_UF2;
                }
                // Brief delay to ensure GPREGRET write completes
                NRFX_DELAY_US(1000);
                NVIC_SystemReset();
            }
        }
    }
}

}  // extern "C"

#endif  // ARDUINO_ARCH_NRF52
