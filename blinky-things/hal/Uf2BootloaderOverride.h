#pragma once
/**
 * Uf2BootloaderOverride.h - Enter UF2 bootloader on 1200-baud touch
 *
 * Overrides TinyUSB's weak tud_cdc_line_state_cb. On 1200-baud DTR drop,
 * writes GPREGRET=0x57 via the SoftDevice API (while SD is still enabled)
 * then resets. Do NOT call sd_softdevice_disable() — it resets the POWER
 * peripheral which can clear GPREGRET. The MBR handles SD state after reset.
 *
 * This matches Nordic's own buttonless DFU implementation in nRF5 SDK:
 * sd_power_gpregret_clr() + sd_power_gpregret_set() + NVIC_SystemReset().
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

                // Write GPREGRET via SoftDevice API while SD is still enabled.
                // Do NOT call sd_softdevice_disable() — it resets the POWER
                // peripheral (documented: "reserved peripherals are reset upon
                // SoftDevice disable") which can clear GPREGRET.
                // The MBR/bootloader handles SD state after reset.
                uint8_t sd_en = 0;
                sd_softdevice_is_enabled(&sd_en);
                if (sd_en) {
                    sd_power_gpregret_clr(0, 0xFF);
                    sd_power_gpregret_set(0, DFU_MAGIC_UF2);
                } else {
                    NRF_POWER->GPREGRET = DFU_MAGIC_UF2;
                }
                __DSB();
                __ISB();
                NVIC_SystemReset();
            }
        }
    }
}

}  // extern "C"

#endif  // ARDUINO_ARCH_NRF52
