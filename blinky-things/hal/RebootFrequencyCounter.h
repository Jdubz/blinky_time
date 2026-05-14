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

    // CRC-protected counter record. The magic alone is a weak integrity check
    // — any 4 random flash bytes matching `0xB007FEED` would pass, and a bit
    // flip from counter=1 → counter=5 would silently trigger DFU recovery on
    // a healthy device. CRC-16/CCITT over magic+counter+reserved catches both
    // (LittleFS gives us atomic writes but not bit-flip detection at rest).
    struct __attribute__((packed)) Record {
        uint32_t magic;     // RECORD_MAGIC
        uint8_t  counter;   // consecutive reboots without markStable
        uint8_t  reserved[3];
        uint16_t crc16;     // CRC-16/CCITT over the preceding 8 bytes
    };
    static_assert(sizeof(Record) == RECORD_SIZE, "Record size mismatch");

    // Plain CRC-16/CCITT (poly 0x1021, init 0xFFFF). Inlined to avoid pulling
    // in a CRC dependency — the bootloader has its own copy at a different
    // address, and the 8-byte input means the cost is negligible.
    inline uint16_t crc16_ccitt(const uint8_t* data, size_t len) {
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < len; i++) {
            crc ^= static_cast<uint16_t>(data[i]) << 8;
            for (int j = 0; j < 8; j++) {
                crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                     : static_cast<uint16_t>(crc << 1);
            }
        }
        return crc;
    }

    // Module-local state — `static` gives internal linkage; safe because only
    // .ino includes this header (matches the SafeBootWatchdog pattern).
    static bool stableMarked_ = false;
    static uint8_t bootCount_ = 0;   // for diagnostics

    /**
     * Read the counter record from flash. Returns 0 if file is missing,
     * unreadable, corrupted, or CRC-mismatched (all treated as "first boot"
     * — a fail-safe default that never spuriously triggers recovery).
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
        const uint16_t expected = crc16_ccitt(
            reinterpret_cast<const uint8_t*>(&rec), sizeof(rec) - sizeof(rec.crc16));
        if (rec.crc16 != expected) {
            Serial.print(F("[FALLBACK] RebootFrequencyCounter: CRC mismatch (read=0x"));
            Serial.print(rec.crc16, HEX);
            Serial.print(F(" expected=0x"));
            Serial.print(expected, HEX);
            Serial.println(F(") — treating as 0"));
            return 0;
        }
        return rec.counter;
    }

    /**
     * Write the counter record to flash. Returns true on full success.
     * Callers in the threshold-trip path MUST check the return value — a
     * silent failure there would leave the counter at threshold and trap
     * the device in a permanent BLE DFU re-entry loop after recovery
     * completes (see checkAndIncrement).
     */
    inline bool writeCounter(uint8_t value) {
        using namespace Adafruit_LittleFS_Namespace;
        Record rec = { RECORD_MAGIC, value, {0, 0, 0}, 0 };
        rec.crc16 = crc16_ccitt(
            reinterpret_cast<const uint8_t*>(&rec), sizeof(rec) - sizeof(rec.crc16));
        // Remove + recreate to ensure clean write (LittleFS handles wear).
        InternalFS.remove(COUNTER_FILE);
        File f(InternalFS);
        if (!f.open(COUNTER_FILE, FILE_O_WRITE)) {
            Serial.println(F("[FALLBACK] RebootFrequencyCounter: file open failed"));
            return false;
        }
        const size_t written = f.write(reinterpret_cast<const uint8_t*>(&rec), sizeof(rec));
        f.close();
        if (written != sizeof(rec)) {
            Serial.print(F("[FALLBACK] RebootFrequencyCounter: short write ("));
            Serial.print(written);
            Serial.print(F("/"));
            Serial.print(sizeof(rec));
            Serial.println(F(" bytes)"));
            return false;
        }
        return true;
    }

    // Optional hook invoked at the moment the boot-counter threshold is
    // reached, immediately before the device enters BLE DFU recovery. Lets
    // the caller (blinky-things.ino) quarantine the stored device config so
    // the next boot won't repeat the crash-loop on the same bad config.
    // Without this hook, a config that crashes the app on every boot
    // (e.g. a misconfigured multi-strand setup that hard-faults during
    // LED-strip init) would keep tripping the threshold and re-entering DFU
    // forever after each recovery flash, with no way to break the cycle
    // wirelessly.
    using OnThresholdHook = void (*)();
    inline OnThresholdHook& onThresholdHook() {
        static OnThresholdHook h = nullptr;
        return h;
    }
    inline void setOnThresholdHook(OnThresholdHook h) { onThresholdHook() = h; }

    /**
     * Increment the boot counter; if threshold reached, enter BLE DFU
     * recovery and never return. MUST be called after `InternalFS.begin()`.
     */
    inline void checkAndIncrement() {
        bootCount_ = readCounter();

        // Promote to uint16_t before adding 1 to make the comparison unambiguous
        // and immune to any future REBOOT_THRESHOLD change crossing 255.
        const uint16_t next = static_cast<uint16_t>(bootCount_) + 1;

        if (next >= REBOOT_THRESHOLD) {
            Serial.print(F("[BOOT] RebootFrequencyCounter threshold reached ("));
            Serial.print(next);
            Serial.print(F("/"));
            Serial.print(REBOOT_THRESHOLD);
            Serial.println(F(") — entering BLE DFU recovery"));

            // Clear the counter BEFORE entering recovery — otherwise a power
            // cycle in DFU mode would leave the counter at threshold, causing
            // a re-entry loop after recovery completes. If the clear FAILS,
            // we must NOT enter DFU: the device would then trap in a permanent
            // DFU re-entry loop on every subsequent power-on. Falling through
            // to a normal boot is safer — the next stable boot will retry the
            // clear during tickStable(), and if the bug persists we'll hit
            // threshold again on the next cycle.
            if (!writeCounter(0)) {
                Serial.println(F("[FALLBACK] RebootFrequencyCounter: clear failed, "
                                 "skipping DFU recovery this boot to avoid permanent loop"));
                return;
            }
            // Invite the caller (typically the .ino) to quarantine state
            // that may have caused the crash-loop — e.g., the stored device
            // config — before we hand control to the DFU bootloader. Best-
            // effort: hook is optional, errors inside are logged by the hook
            // itself, and we proceed to DFU either way.
            if (onThresholdHook()) {
                onThresholdHook()();
            }
            SafeBootWatchdog::enterBleDfuBootloader();  // Never returns
        }

        writeCounter(next);  // failure logged inside; non-fatal at this step
        Serial.print(F("[BOOT] RebootFrequencyCounter: "));
        Serial.print(next);
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
        Serial.print(F("[BOOT] RebootFrequencyCounter cleared ("));
        Serial.print(STABLE_UPTIME_MS / 60000UL);
        Serial.println(F("min stable uptime)"));
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
