#pragma once
/**
 * Uf2BootloaderOverride.h - Reliable bootloader entry from application code
 *
 * Overrides TinyUSB's weak tud_cdc_line_state_cb to enter UF2 mode on
 * 1200-baud touch. Uses direct jump to bootloader after manually disabling
 * the USB peripheral — this preserves GPREGRET 100% (no reset needed).
 *
 * Key insight: NVIC_SystemReset() is unreliable for GPREGRET because
 * sd_softdevice_disable() resets the POWER peripheral as a side effect.
 * Direct jump avoids the reset entirely. To make it work for UF2 mode
 * (which needs USB mass storage), we manually disable the USBD peripheral
 * so the bootloader can reinitialize it cleanly.
 */

#ifdef ARDUINO_ARCH_NRF52

#include <Arduino.h>
#include <class/cdc/cdc_device.h>

extern "C" {
  #include <nrf_sdm.h>
  #include <nrf_soc.h>
}

/**
 * Enter bootloader via direct jump. 100% reliable GPREGRET preservation.
 *
 * Works for BOTH UF2 and BLE DFU modes because we manually disable the
 * USB peripheral before jumping, giving the bootloader a clean USB state.
 *
 * @param magic GPREGRET value (0x57=UF2, 0xA8=BLE DFU OTA reset)
 */
static inline void enterBootloaderDirect(uint8_t magic) __attribute__((noreturn));
static inline void enterBootloaderDirect(uint8_t magic) {
    // 1. Disable SoftDevice (releases peripheral protection)
    uint8_t sd_en = 0;
    sd_softdevice_is_enabled(&sd_en);
    if (sd_en) {
        sd_softdevice_disable();
    }

    // 2. Write GPREGRET AFTER SD disable completes.
    //    sd_softdevice_disable() resets "all peripherals used by the SD"
    //    including the POWER peripheral. Writing before disable is lost.
    __DSB(); __ISB();
    NRF_POWER->GPREGRET = magic;
    __DSB(); __ISB();

    // 3. Disable USB peripheral so bootloader can reinit for mass storage.
    //    Without this, the bootloader inherits app's CDC configuration and
    //    USB mass storage init fails silently.
    NRF_USBD->USBPULLUP = 0;
    NRF_USBD->ENABLE = 0;
    // Busy-wait for USB disable to complete (typically <10us)
    volatile int i = 0;
    while (NRF_USBD->ENABLE && i < 10000) { i++; }

    // 4. Disable all interrupts
    for (int j = 0; j < 8; j++) {
        NVIC->ICER[j] = 0xFFFFFFFF;
        NVIC->ICPR[j] = 0xFFFFFFFF;
    }

    // 5. Direct jump to bootloader (preserves GPREGRET 100%)
    uint32_t bl = NRF_UICR->NRFFW[0];
    if (bl != 0xFFFFFFFF) {
        __set_MSP(*((uint32_t *)bl));
        __set_CONTROL(0);
        __ISB();
        ((void (*)(void))(*((uint32_t *)(bl + 4))))();
    }

    // Fallback: if UICR not set, use system reset (less reliable)
    NVIC_SystemReset();
    __builtin_unreachable();
}

extern "C" {

void tud_cdc_line_state_cb(uint8_t instance, bool dtr, bool rts) {
    (void)rts;

    if (!dtr) {
        if (instance == 0) {
            cdc_line_coding_t coding;
            tud_cdc_get_line_coding(&coding);

            if (coding.bit_rate == 1200) {
                enterBootloaderDirect(0x57);  // UF2 mass storage
            }
        }
    }
}

}  // extern "C"

#endif  // ARDUINO_ARCH_NRF52
