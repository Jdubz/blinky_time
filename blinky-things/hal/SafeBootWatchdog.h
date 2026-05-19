#pragma once
/**
 * SafeBootWatchdog — Hardware watchdog + auto-recovery for nRF52840
 *
 * Prevents permanent device bricking by combining:
 * 1. GPREGRET2-based boot counter (persists across ALL reset types)
 * 2. Hardware WDT (catches HardFaults, infinite loops, heap exhaustion)
 * 3. Auto-entry to bootloader after consecutive boot failures
 *
 * How it works:
 * - On each boot, GPREGRET2 is incremented (persists across WDT/soft reset)
 * - If GPREGRET2 >= BOOT_FAIL_THRESHOLD, device enters bootloader
 * - Hardware WDT starts with 15s timeout — catches any hang during init
 * - After successful init, markStable() clears the counter
 * - Main loop feeds the WDT every iteration
 *
 * Recovery mode (compile-time):
 *   Default: UF2 mass storage (0x57) — user copies .uf2 file via USB
 *   SAFEBOOT_BLE_DFU_RECOVERY: BLE DFU (0xA8) — fleet server pushes firmware
 *     wirelessly. Use this for physically installed devices without USB access.
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
     * Enter bootloader via RAM magic (never returns).
     *
     * Writes a magic value to 0x20007F7C (same address as bootloader's
     * double-reset detection). RAM survives system reset, unlike GPREGRET
     * which can be cleared by USB hub port power-cycling during reset.
     * Custom bootloader checks this address BEFORE GPREGRET.
     *
     * Also clears boot counter (GPREGRET2) so the device doesn't
     * immediately re-enter recovery on next normal boot.
     */
    inline void enterBootloaderWithMagic(uint8_t magic) {
        // RAM-based entry (primary — reliable through USB hubs)
        volatile uint32_t* bootloader_ram = (volatile uint32_t*)0x20007F7C;
        if (magic == 0x57) {
            *bootloader_ram = 0x5A1AD5;    // DFU_DBL_RESET_MAGIC — RAM path (stock + custom bootloader)
            NRF_POWER->GPREGRET = 0x57;    // GPREGRET path — stock bootloader fallback, survives hub power-cycle
        } else if (magic == 0xA8) {
            *bootloader_ram = 0xBEEF00A8;  // BLE DFU mode (custom bootloader only)
            NRF_POWER->GPREGRET = 0xA8;    // DFU_MAGIC_OTA_RESET — stock bootloader fallback
        }

        // Clear boot counter
        uint8_t sd_en = 0;
        sd_softdevice_is_enabled(&sd_en);
        if (sd_en) {
            sd_power_gpregret_clr(1, 0xFF);
        } else {
            NRF_POWER->GPREGRET2 = 0;
        }

        __DSB(); __ISB();
        NVIC_SystemReset();
    }

    /**
     * Enter UF2 mass storage bootloader (never returns).
     * User copies .uf2 file via USB to recover.
     */
    inline void enterUf2Bootloader() {
        enterBootloaderWithMagic(0x57);
    }

    /**
     * Enter BLE DFU bootloader (never returns).
     * Fleet server pushes firmware wirelessly to recover.
     * Use for physically installed devices without USB access.
     */
    inline void enterBleDfuBootloader() {
        enterBootloaderWithMagic(0xA8);
    }

    /**
     * Enter recovery bootloader (never returns).
     *
     * Prefer UF2 (USB mass-storage, ~30 s host-side recovery) when a
     * USB cable is currently attached — VBUSDETECT high means a host
     * machine is plugged in, so a UF2 drop will be visible AND applied
     * in seconds instead of waiting through the ~5.5-minute BLE-DFU
     * transfer. Sealed sculpture devices with no USB access fall
     * through to BLE-DFU as before.
     *
     * OPEN_ISSUES §3.2 / [[project-bl-prefer-uf2-when-usb]]. UF2 path
     * remains valid for sealed devices via physical double-tap reset
     * as a manual fallback when neither this auto-trigger nor the
     * fleet server can reach them.
     *
     * Known limitation: if a device crash-loops AND neither USB nor
     * BLE is reachable (out of range, interference, fleet server
     * down, USB cable unplugged), there is no automatic recovery
     * path. Sealed devices with inaccessible reset buttons would
     * require physical access or SWD in that case.
     */
    inline void enterRecoveryBootloader() {
        // USBREGSTATUS.VBUSDETECT is a real-time hardware bit: 1 iff
        // a USB cable is currently providing VBUS. The bit reflects
        // physical electrical state, not enumeration — so an
        // unenumerated cable (e.g. a wall charger) still counts.
        // That's the right behaviour here: any host capable of
        // accepting a UF2 mass-storage device will hold VBUS up.
        // The fall-through to BLE for sealed devices (no VBUS) keeps
        // the autonomous-recovery contract for installed sculptures.
        const bool usb_present =
            (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
        if (usb_present) {
            enterUf2Bootloader();
        } else {
            enterBleDfuBootloader();
        }
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
            // Recovery mode selected at compile time (UF2 or BLE DFU).
            enterRecoveryBootloader();  // Never returns
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
