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
 * TinyUSB library. Our version disables the SoftDevice before writing
 * GPREGRET and resetting — without this, the SD's reset handler clears
 * GPREGRET and the bootloader boots straight to the app instead of UF2.
 */

#ifdef ARDUINO_ARCH_NRF52

#include <Arduino.h>
#include <class/cdc/cdc_device.h>

extern "C" {
  #include <nrf_sdm.h>
  #include <nrf_soc.h>
}

extern "C" {

void tud_cdc_line_state_cb(uint8_t instance, bool dtr, bool rts) {
    (void)rts;

    if (!dtr) {
        if (instance == 0) {
            cdc_line_coding_t coding;
            tud_cdc_get_line_coding(&coding);

            if (coding.bit_rate == 1200) {
                const uint8_t DFU_MAGIC_UF2 = 0x57;

                // CRITICAL: Disable SoftDevice BEFORE writing GPREGRET.
                // Without this, the SD's reset handler clears GPREGRET during
                // NVIC_SystemReset(), causing the bootloader to boot the app
                // instead of entering UF2 mode. This was the root cause of
                // uf2_upload.py's 1200-baud touch consistently failing.
                uint8_t sd_en = 0;
                sd_softdevice_is_enabled(&sd_en);
                if (sd_en) {
                    sd_power_gpregret_clr(0, 0xFF);
                    sd_power_gpregret_set(0, DFU_MAGIC_UF2);
                    sd_softdevice_disable();
                }
                // Write directly after SD is disabled
                NRF_POWER->GPREGRET = DFU_MAGIC_UF2;
                NRFX_DELAY_US(1000);
                NVIC_SystemReset();
            }
        }
    }
}

}  // extern "C"

#endif  // ARDUINO_ARCH_NRF52
