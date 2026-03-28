#pragma once
/**
 * SafeBootWatchdog — Hardware watchdog + auto-recovery for nRF52840
 *
 * Prevents permanent device bricking by combining:
 * 1. GPREGRET2-based boot counter (persists across ALL reset types)
 * 2. Hardware WDT (catches HardFaults, infinite loops, heap exhaustion)
 * 3. Auto-entry to UF2 bootloader after consecutive boot failures
 *
 * How it works:
 * - On each boot, GPREGRET2 is incremented (persists across WDT/soft reset)
 * - If GPREGRET2 >= BOOT_FAIL_THRESHOLD, device enters UF2 bootloader
 * - Hardware WDT starts with 15s timeout — catches any hang during init
 * - After successful init, markStable() clears the counter
 * - Main loop feeds the WDT every iteration
 *
 * Recovery flow for users:
 *   Device crashes → WDT fires → reset → counter incremented
 *   After 3 resets → auto-enters UF2 bootloader → copy firmware.uf2 → fixed
 *
 * GPREGRET2 is cleared on power-on reset (hardware behavior), so a USB
 * power cycle always gives the device a fresh start.
 *
 * Usage:
 *   void setup() {
 *     SafeBootWatchdog::begin();     // MUST be first — before any allocations
 *     // ... rest of setup ...
 *     SafeBootWatchdog::markStable(); // After successful init
 *   }
 *   void loop() {
 *     SafeBootWatchdog::feed();       // Feed WDT every iteration
 *     // ...
 *   }
 */

#ifdef ARDUINO_ARCH_NRF52

#include <Arduino.h>

extern "C" {
  #include <nrf_sdm.h>
  #include <nrf_soc.h>
}

namespace SafeBootWatchdog {

    // Enter UF2 bootloader after this many consecutive failed boots
    static constexpr uint8_t BOOT_FAIL_THRESHOLD = 3;

    // WDT timeout in seconds — must be longer than worst-case init time
    // NN init (AllocateTensors): ~1s, LED test: ~1s, serial delay: ~1s
    // 15s gives generous headroom
    static constexpr uint32_t WDT_TIMEOUT_SECONDS = 15;

    // Track whether we started the WDT (cannot be stopped once started).
    // static gives internal linkage — each TU gets its own copy. This is safe
    // because only blinky-things.ino includes this header. If included from
    // multiple .cpp files, move these to a .cpp file or use inline (C++17).
    static bool wdtStarted_ = false;
    static uint8_t bootCount_ = 0;

    /**
     * Read GPREGRET2 via SoftDevice API (safe) or direct register access.
     */
    inline uint8_t readBootCounter() {
        uint8_t sd_en = 0;
        sd_softdevice_is_enabled(&sd_en);
        if (sd_en) {
            uint32_t val = 0;
            sd_power_gpregret_get(1, &val);  // index 1 = GPREGRET2
            return static_cast<uint8_t>(val & 0xFF);
        }
        return static_cast<uint8_t>(NRF_POWER->GPREGRET2 & 0xFF);
    }

    /**
     * Write GPREGRET2 via SoftDevice API (safe) or direct register access.
     */
    inline void writeBootCounter(uint8_t val) {
        uint8_t sd_en = 0;
        sd_softdevice_is_enabled(&sd_en);
        if (sd_en) {
            sd_power_gpregret_clr(1, 0xFF);
            sd_power_gpregret_set(1, val);
        } else {
            NRF_POWER->GPREGRET2 = val;
        }
    }

    /**
     * Enter UF2 mass storage bootloader (never returns).
     *
     * Always enters UF2 mode (0x57, USB mass storage) — NOT BLE DFU.
     * UF2 is the safest recovery: user just copies a .uf2 file via USB.
     * BLE DFU (0xA8) has no automatic exit and can leave the device
     * stuck if the BLE connection fails. UF2 always provides a working
     * USB interface for recovery.
     */
    inline void enterUf2Bootloader() {
        const uint8_t DFU_MAGIC_UF2 = 0x57;
        uint8_t sd_en = 0;
        sd_softdevice_is_enabled(&sd_en);
        if (sd_en) {
            sd_power_gpregret_clr(0, 0xFF);
            sd_power_gpregret_set(0, DFU_MAGIC_UF2);
            // Clear boot counter so new firmware gets a clean start
            sd_power_gpregret_clr(1, 0xFF);
            // Disable SoftDevice before reset — without this, the SD's
            // reset handler clears GPREGRET and the bootloader boots the
            // app instead of entering UF2 mode.
            sd_softdevice_disable();
        }
        NRF_POWER->GPREGRET = DFU_MAGIC_UF2;
        NRF_POWER->GPREGRET2 = 0;
        NRFX_DELAY_US(1000);
        NVIC_SystemReset();
    }

    /**
     * Start the hardware WDT. Once started, it CANNOT be stopped (nRF52 design).
     * Must call feed() regularly or the device will reset.
     */
    inline void startWdt() {
        if (wdtStarted_) return;

        NRF_WDT->CONFIG = 1;  // Run during SLEEP, pause during HALT (debug)
        NRF_WDT->CRV = WDT_TIMEOUT_SECONDS * 32768;  // 32.768 kHz clock
        NRF_WDT->RREN = 1;    // Enable reload register 0
        NRF_WDT->TASKS_START = 1;
        wdtStarted_ = true;
    }

    /**
     * Feed (kick) the hardware WDT. Must be called at least once per
     * WDT_TIMEOUT_SECONDS or the device will reset.
     */
    inline void feed() {
        if (wdtStarted_) {
            NRF_WDT->RR[0] = 0x6E524635;  // WDT reload magic value
        }
    }

    /**
     * Check boot counter and start WDT. MUST be the first call in setup().
     *
     * If too many consecutive boot failures detected, enters UF2 bootloader
     * automatically — the user just needs to copy new firmware.
     */
    inline void begin() {
        bootCount_ = readBootCounter();

        if (bootCount_ >= BOOT_FAIL_THRESHOLD) {
            // Too many consecutive failed boots — enter bootloader for recovery.
            // The UF2 bootloader is safe: invalid firmware is silently rejected.
            enterUf2Bootloader();  // Never returns
        }

        // Increment boot counter BEFORE doing anything else.
        // If the next lines crash, the counter is already incremented.
        writeBootCounter(bootCount_ + 1);

        // Start hardware WDT — if setup() hangs, WDT resets device
        startWdt();
        feed();  // Initial feed to start the timeout window
    }

    /**
     * Mark boot as successful. Clears the boot counter and feeds the WDT.
     * Call this after all critical initialization is complete.
     */
    inline void markStable() {
        writeBootCounter(0);
        feed();
    }

    /**
     * Get the boot attempt counter at startup (for diagnostics).
     */
    inline uint8_t getBootCount() { return bootCount_; }

    /**
     * Check if WDT is running.
     */
    inline bool isWdtActive() { return wdtStarted_; }

}  // namespace SafeBootWatchdog

#else

// Stub for non-nRF52 platforms
namespace SafeBootWatchdog {
    inline void begin() {}
    inline void feed() {}
    inline void markStable() {}
    inline uint8_t getBootCount() { return 0; }
    inline bool isWdtActive() { return false; }
}

#endif  // ARDUINO_ARCH_NRF52
