#pragma once
/**
 * RebootFrequencyCounter — flash-backed crash-loop detector for sealed devices.
 *
 * Complements SafeBootWatchdog (`hal/SafeBootWatchdog.h`):
 *
 *   SafeBootWatchdog (GPREGRET2)         RebootFrequencyCounter (flash)
 *   ─────────────────────────────        ──────────────────────────────
 *   Survives:  WDT reset, soft reset    Survives:  any reset, power cycle
 *   Wiped by:  power-on reset           Wiped by:  only this module
 *   Catches:   setup() crashes within   Catches:   runtime crashes that
 *              a single power session              survive `markStable()`,
 *                                                  even across power cycles
 *
 * Why both: a runtime bug that crashes the device after the 60-second
 * `markStable()` window resets GPREGRET2 to 0 every time the boot succeeds.
 * GPREGRET2 also clears on power-on reset, so a sculpture that loses power
 * between crashes has no in-RAM history. Without flash-backed state, a
 * crash-loop that defeats those mechanisms never triggers BLE DFU recovery.
 *
 * Behavior:
 *   1. `checkAndIncrement()` runs early in setup (after LittleFS is ready).
 *      Reads counter from flash, increments by 1, writes back.
 *   2. If post-increment counter >= REBOOT_THRESHOLD, enters BLE DFU
 *      bootloader via SafeBootWatchdog. Never returns.
 *   3. `markStable()` is called from loop() once millis() >= STABLE_UPTIME_MS.
 *      Writes counter=0 to flash.
 *
 * Failure modes (deliberately fail-safe):
 *   - File missing or unreadable → counter treated as 0 → device boots
 *     normally. No spurious recovery on first install or LittleFS corruption.
 *   - Flash write fails → silent (next boot will see stale counter and either
 *     boot fine or trigger recovery one boot later). No fatal halt.
 *
 * Storage:
 *   - LittleFS file at `/.boot_freq` (10 bytes: 4B magic, 1B counter, 5B reserved)
 *   - Magic value distinguishes valid record from uninitialized / corrupt data.
 *
 * Usage:
 *   void setup() {
 *     SafeBootWatchdog::begin();      // GPREGRET2 check (first)
 *     ...
 *     configStorage.begin();          // Initializes LittleFS
 *     RebootFrequencyCounter::checkAndIncrement();  // Flash check (second)
 *     ...
 *   }
 *   void loop() {
 *     ...
 *     RebootFrequencyCounter::tickStable(millis());  // Idempotent
 *     ...
 *   }
 *
 * See docs/SCULPTURE_BLE_RECOVERY_PLAN.md (F4) for design rationale.
 */

#ifdef ARDUINO_ARCH_NRF52

#include <Arduino.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include "SafeBootWatchdog.h"

namespace RebootFrequencyCounter {

    // Enter BLE DFU recovery once this many consecutive boots have occurred
    // without a `markStable()` call between them. Suggested 5 — gives the
    // app several boots to try to recover on its own (e.g. user-initiated
    // reboots, occasional WDT trips) before forcing recovery mode.
    static constexpr uint8_t REBOOT_THRESHOLD = 5;

    // Uptime threshold (ms) before counter is cleared. Should be comfortably
    // longer than the worst legitimate "device runs OK for a while then bug"
    // window. 5 minutes catches most runtime crash bugs while still allowing
    // the fleet server time to observe and intervene.
    static constexpr uint32_t STABLE_UPTIME_MS = 5UL * 60UL * 1000UL;

    static constexpr const char* COUNTER_FILE = "/.boot_freq";
    static constexpr uint32_t RECORD_MAGIC    = 0xB007FEEDUL;
    static constexpr size_t   RECORD_SIZE     = 10;

    struct __attribute__((packed)) Record {
        uint32_t magic;     // RECORD_MAGIC
        uint8_t  counter;   // consecutive reboots without markStable
        uint8_t  reserved[5];
    };
    static_assert(sizeof(Record) == RECORD_SIZE, "Record size mismatch");

    // Module-local state — `static` gives internal linkage; safe because only
    // .ino includes this header (matches the SafeBootWatchdog pattern).
    static bool stableMarked_ = false;
    static uint8_t bootCount_ = 0;   // for diagnostics

    /**
     * Read the counter record from flash. Returns 0 if file is missing,
     * unreadable, or corrupted (any of these are treated as "first boot").
     */
    inline uint8_t readCounter() {
        using namespace Adafruit_LittleFS_Namespace;
        File f(InternalFS);
        if (!f.open(COUNTER_FILE, FILE_O_READ)) {
            return 0;
        }
        Record rec;
        size_t n = f.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec));
        f.close();
        if (n != sizeof(rec) || rec.magic != RECORD_MAGIC) {
            return 0;
        }
        return rec.counter;
    }

    /**
     * Write the counter record to flash. Silent on failure — the next boot
     * will either see the stale value (boot proceeds normally) or trigger
     * recovery one boot later. No halt on write failure.
     */
    inline void writeCounter(uint8_t value) {
        using namespace Adafruit_LittleFS_Namespace;
        Record rec = { RECORD_MAGIC, value, {0, 0, 0, 0, 0} };
        // Remove + recreate to ensure clean write (LittleFS handles wear).
        InternalFS.remove(COUNTER_FILE);
        File f(InternalFS);
        if (!f.open(COUNTER_FILE, FILE_O_WRITE)) {
            Serial.println(F("[FALLBACK] RebootFrequencyCounter: file open failed"));
            return;
        }
        f.write(reinterpret_cast<const uint8_t*>(&rec), sizeof(rec));
        f.close();
    }

    /**
     * Increment the boot counter; if threshold reached, enter BLE DFU
     * recovery and never return. MUST be called after `InternalFS.begin()`.
     */
    inline void checkAndIncrement() {
        bootCount_ = readCounter();

        if (bootCount_ + 1 >= REBOOT_THRESHOLD) {
            Serial.print(F("[BOOT] RebootFrequencyCounter threshold reached ("));
            Serial.print((unsigned)bootCount_ + 1);
            Serial.print(F("/"));
            Serial.print(REBOOT_THRESHOLD);
            Serial.println(F(") — entering BLE DFU recovery"));
            // Clear the counter BEFORE entering recovery — otherwise a power
            // cycle in DFU mode would leave the counter at threshold, causing
            // a re-entry loop after recovery completes.
            writeCounter(0);
            SafeBootWatchdog::enterBleDfuBootloader();  // Never returns
        }

        writeCounter(bootCount_ + 1);
        Serial.print(F("[BOOT] RebootFrequencyCounter: "));
        Serial.print((unsigned)(bootCount_ + 1));
        Serial.print(F("/"));
        Serial.println(REBOOT_THRESHOLD);
    }

    /**
     * Call from loop() every iteration; idempotent. Clears the counter once
     * the device has been up for `STABLE_UPTIME_MS`.
     */
    inline void tickStable(uint32_t nowMs) {
        if (stableMarked_) return;
        if (nowMs < STABLE_UPTIME_MS) return;
        writeCounter(0);
        stableMarked_ = true;
        Serial.println(F("[BOOT] RebootFrequencyCounter cleared (5min stable uptime)"));
    }

    /**
     * Diagnostic: boot counter at startup (before increment).
     */
    inline uint8_t getBootCount() { return bootCount_; }

}  // namespace RebootFrequencyCounter

#else

// Stub for non-nRF52 platforms
namespace RebootFrequencyCounter {
    inline void checkAndIncrement() {}
    inline void tickStable(uint32_t) {}
    inline uint8_t getBootCount() { return 0; }
}

#endif  // ARDUINO_ARCH_NRF52
