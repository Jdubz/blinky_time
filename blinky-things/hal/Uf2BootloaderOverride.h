#pragma once
/**
 * Uf2BootloaderOverride.h - Enter UF2 bootloader on 1200-baud touch
 *
 * Overrides TinyUSB's weak tud_cdc_line_state_cb. On 1200-baud DTR drop,
 * disables SoftDevice, writes GPREGRET=0x57, and resets. The GPREGRET
 * write is AFTER sd_softdevice_disable() because the disable resets the
 * POWER peripheral (documented side effect) which clears any prior writes.
 *
 * This is intermittent (~50-80% success) due to MBR interaction with
 * GPREGRET during reset. The upload tool (uf2_upload.py) retries up to
 * 5 times to compensate.
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

                // 1. Disable SoftDevice FIRST — releases peripheral protection
                //    and resets POWER peripheral (clears any prior GPREGRET writes)
                uint8_t sd_en = 0;
                sd_softdevice_is_enabled(&sd_en);
                if (sd_en) {
                    sd_softdevice_disable();
                }

                // 2. Write GPREGRET AFTER SD disable completes
                __DSB();
                __ISB();
                NRF_POWER->GPREGRET = DFU_MAGIC_UF2;

                // 3. Ensure write commits before reset
                __DSB();
                __ISB();
                NVIC_SystemReset();
            }
        }
    }
}

}  // extern "C"

#endif  // ARDUINO_ARCH_NRF52
