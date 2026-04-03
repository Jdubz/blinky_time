#pragma once
/**
 * Uf2BootloaderOverride.h - Enter UF2 bootloader on 1200-baud touch
 *
 * Overrides TinyUSB's weak tud_cdc_line_state_cb. On 1200-baud DTR drop,
 * writes a magic value to RAM and resets. The custom bootloader checks this
 * RAM location before GPREGRET, providing reliable bootloader entry even
 * through USB hubs that power-cycle ports during reset.
 *
 * RAM at 0x20007F7C survives system reset — proven by the bootloader's own
 * double-reset detection which uses the same address.
 */

#ifdef ARDUINO_ARCH_NRF52

#include <Arduino.h>
#include <class/cdc/cdc_device.h>

// RAM-based bootloader entry: same address as DFU_DBL_RESET_MEM in the
// Adafruit bootloader. Values are distinct from DFU_DBL_RESET_MAGIC (0x5A1AD5).
#define BOOTLOADER_RAM_ADDR    ((volatile uint32_t*)0x20007F7C)
#define BOOTLOADER_RAM_UF2     0xBEEF0057
#define BOOTLOADER_RAM_BLE     0xBEEF00A8

extern "C" {

void tud_cdc_line_state_cb(uint8_t instance, bool dtr, bool rts) {
    (void)rts;

    if (!dtr) {
        if (instance == 0) {
            cdc_line_coding_t coding;
            tud_cdc_get_line_coding(&coding);

            if (coding.bit_rate == 1200) {
                *BOOTLOADER_RAM_ADDR = BOOTLOADER_RAM_UF2;
                __DSB();
                __ISB();
                NVIC_SystemReset();
            }
        }
    }
}

}  // extern "C"

#endif  // ARDUINO_ARCH_NRF52
