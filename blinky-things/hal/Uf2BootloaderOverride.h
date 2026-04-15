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
// Adafruit bootloader. UF2 uses DFU_DBL_RESET_MAGIC (0x5A1AD5) — recognized
// by both the stock Adafruit v0.6.1 bootloader and the custom bootloader
// (which checks 0x5A1AD5 as a fallback after its own magic). Writing to RAM
// avoids GPREGRET which USB hubs may clear during port power-cycle on reset.
#define BOOTLOADER_RAM_ADDR    ((volatile uint32_t*)0x20007F7C)
#define BOOTLOADER_RAM_UF2     0x5A1AD5   // DFU_DBL_RESET_MAGIC — stock + custom bootloader
#define BOOTLOADER_RAM_BLE     0xBEEF00A8 // Custom bootloader only (BLE DFU)

extern "C" {

void tud_cdc_line_state_cb(uint8_t instance, bool dtr, bool rts) {
    (void)rts;

    if (!dtr) {
        if (instance == 0) {
            cdc_line_coding_t coding;
            tud_cdc_get_line_coding(&coding);

            if (coding.bit_rate == 1200) {
                *BOOTLOADER_RAM_ADDR = BOOTLOADER_RAM_UF2;  // RAM path (custom + stock bootloader)
                NRF_POWER->GPREGRET = 0x57;                  // GPREGRET path (stock bootloader fallback, survives hub power-cycle)
                __DSB();
                __ISB();
                NVIC_SystemReset();
            }
        }
    }
}

}  // extern "C"

#endif  // ARDUINO_ARCH_NRF52
