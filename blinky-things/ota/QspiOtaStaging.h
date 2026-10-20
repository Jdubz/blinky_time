#pragma once
/**
 * QspiOtaStaging — QSPI flash staging area for safe OTA firmware updates.
 *
 * Firmware is written to the external 2 MB QSPI flash (P25Q16H) while the
 * app is running. Only after the complete image passes CRC validation does
 * the bootloader copy it to internal flash (triggered by GPREGRET=0xCC).
 * If anything fails, the old app boots normally.
 *
 * QSPI layout:
 *   0x000000: 8-byte header {uint32 size, uint16 crc16, uint16 magic}
 *   0x001000: Firmware binary (4KB-aligned)
 *
 * Serial/BLE commands (SerialConsole::handleOtaCommand):
 *   ota manifest <hex>             — Signed manifest (OtaManifest.h). Nothing
 *                                    is staged without a valid signature from
 *                                    the release key, our board, and a newer
 *                                    build (or a signed emergency rollback).
 *   ota chunk <off> <crc16> <b64>  — In-order image chunk (<= MAX_CHUNK bytes),
 *                                    hashed as it lands.
 *   ota commit                     — Re-read the staged image, check its
 *                                    SHA-512, write the header, reset into the
 *                                    bootloader's apply path.
 *   ota status                     — One JSON line (state, next offset, MTU).
 *   ota abort                      — Clear the staging header.
 *   ota selftest                   — Copy the running firmware to QSPI and
 *                                    verify it (never arms the bootloader).
 * There is no unsigned way to stage an image: a web page or any nearby BLE
 * central can reach this console (docs/WEB_FLASHER.md, "Signed OTA").
 */

#ifdef ARDUINO_ARCH_NRF52

#include <Arduino.h>
#include "../hal/SafeBootWatchdog.h"
#include "../types/Version.h"
#include "OtaKeys.h"
#include "OtaManifest.h"

extern "C" {
  #include <nrf_sdm.h>
  #include <nrf_soc.h>
  #include <nrfx_qspi.h>
}

// cppcheck-suppress ctuOneDefinitionRuleViolation
class QspiOtaStaging {
public:
    // Must match bootloader's qspi_staging_header_t
    static constexpr uint32_t HEADER_ADDR    = 0x000000;
    static constexpr uint32_t FIRMWARE_ADDR  = 0x001000;
    static constexpr uint16_t HEADER_MAGIC   = 0xB10C;
    static constexpr uint32_t SECTOR_SIZE    = 4096;
    static constexpr uint32_t MAX_FW_SIZE    = 0xCD000;  // 820 KB absolute max

    static constexpr uint8_t GPREGRET_QSPI_APPLY = 0xCC;

    struct __attribute__((packed, aligned(4))) StagingHeader {
        uint32_t size;
        uint16_t crc16;
        uint16_t magic;
    };

    /**
     * Initialize QSPI flash. Call once during setup() or on first OTA command.
     * Uses nrfx_qspi directly to avoid SdFat namespace collision with LittleFS.
     * Returns true if QSPI flash is accessible.
     */
    bool begin() {
        if (ready_) return true;

        // XIAO nRF52840 Sense QSPI pins (physical P0.xx)
        nrfx_qspi_config_t cfg = {
            .xip_offset = 0,
            .pins = {
                .sck_pin = NRF_GPIO_PIN_MAP(0, 21),
                .csn_pin = NRF_GPIO_PIN_MAP(0, 25),
                .io0_pin = NRF_GPIO_PIN_MAP(0, 20),
                .io1_pin = NRF_GPIO_PIN_MAP(0, 24),
                .io2_pin = NRF_GPIO_PIN_MAP(0, 22),
                .io3_pin = NRF_GPIO_PIN_MAP(0, 23),
            },
            .prot_if = {
                .readoc   = NRF_QSPI_READOC_READ4O,
                .writeoc  = NRF_QSPI_WRITEOC_PP4O,
                .addrmode = NRF_QSPI_ADDRMODE_24BIT,
            },
            .phy_if = {
                .sck_delay = 10,
                .dpmen     = false,
                .spi_mode  = NRF_QSPI_MODE_0,
                .sck_freq  = NRF_QSPI_FREQ_32MDIV16,  // 2 MHz (conservative)
            },
            .irq_priority = 7,
        };

        if (nrfx_qspi_init(&cfg, NULL, NULL) != NRFX_SUCCESS) {
            return false;
        }
        if (!ensureQuadEnable()) {
            nrfx_qspi_uninit();
            return false;
        }
        ready_ = true;
        return true;
    }

    /** Status register 2 as last read by ensureQuadEnable() (QE = bit 1). */
    uint8_t statusReg2() const { return sr2_; }

    void end() {
        if (ready_) {
            nrfx_qspi_uninit();
            ready_ = false;
        }
    }

    bool isReady() const { return ready_; }

    /**
     * CRC-16/CCITT — must match bootloader's crc16_compute() exactly.
     * Initial value 0xFFFF, polynomial embedded in the byte-swap algorithm.
     */
    static uint16_t crc16(const uint8_t* data, uint32_t size, uint16_t initial = 0xFFFF) {
        uint16_t crc = initial;
        for (uint32_t i = 0; i < size; i++) {
            crc = (uint8_t)(crc >> 8) | (crc << 8);
            crc ^= data[i];
            crc ^= (uint8_t)(crc & 0xFF) >> 4;
            crc ^= (crc << 8) << 4;
            crc ^= ((crc & 0xFF) << 4) << 1;
        }
        return crc;
    }

    /**
     * Read the current staging header from QSPI.
     */
    bool readHeader(StagingHeader& hdr) {
        if (!ready_) return false;
        // nrfx_qspi_read requires 4-byte aligned buffer — StagingHeader is 8 bytes (OK)
        return nrfx_qspi_read(&hdr, sizeof(hdr), HEADER_ADDR) == NRFX_SUCCESS;
    }

    /** Erase a 4 KB sector by sector number (0-based). */
    bool eraseSector(uint32_t sectorNum) {
        return nrfx_qspi_erase(NRF_QSPI_ERASE_LEN_4KB, sectorNum * SECTOR_SIZE) == NRFX_SUCCESS;
    }

    /** Read from QSPI flash. Buffer must be 4-byte aligned, len multiple of 4. */
    bool qspiRead(void* buf, uint32_t len, uint32_t addr) {
        return nrfx_qspi_read(buf, (len + 3) & ~3, addr) == NRFX_SUCCESS;
    }

    /** Write to QSPI flash. Buffer must be 4-byte aligned, len multiple of 4. */
    bool qspiWrite(const void* buf, uint32_t len, uint32_t addr) {
        return nrfx_qspi_write(buf, (len + 3) & ~3, addr) == NRFX_SUCCESS;
    }

    /**
     * Self-test: copy current firmware from internal flash to QSPI staging,
     * read back and verify CRC. Non-destructive (doesn't write the header
     * magic, so bootloader won't apply it).
     *
     * This tests the full QSPI write/read/CRC pipeline.
     */
    bool selfTest(Print& out) {
        reset();  // the self-test overwrites the staging area
        if (!ready_) {
            out.println(F("ERR QSPI not initialized"));
            return false;
        }

        // App start address. Hardcoded for nRF52840 with S140 v7.3.0.
        // This matches the bootloader's CODE_REGION_1_START and the linker script.
        // SD_SIZE_GET() reads from the SoftDevice info struct but may return garbage
        // when called from app context (the address 0x2008 can be remapped by MBR).
        static constexpr uint32_t APP_START = 0x27000;
        uint32_t appStart = APP_START;

        // Sanity check: first word at appStart should be a valid stack pointer
        uint32_t sp = *(volatile uint32_t*)appStart;
        if ((sp & 0xFFF00000) != 0x20000000) {
            out.print(F("ERR no valid app at 0x"));
            out.print(appStart, HEX);
            out.print(F(" (SP=0x"));
            out.print(sp, HEX);
            out.println(F(")"));
            return false;
        }

        // Find firmware size by scanning backwards from max for non-0xFF pages
        // (simpler and safer than reading bootloader settings)
        uint32_t appEnd = 0xF4000;  // BOOTLOADER_REGION_START
        uint32_t fwSize = 0;
        for (uint32_t addr = appEnd - 4; addr >= appStart; addr -= 4) {
            if ((addr & 0x3FFF) == 0) SafeBootWatchdog::feed();  // Every 16K
            if (*(volatile uint32_t*)addr != 0xFFFFFFFF) {
                fwSize = (addr + 4) - appStart;
                // Round up to 4-byte alignment
                fwSize = (fwSize + 3) & ~3;
                break;
            }
        }

        if (fwSize == 0 || fwSize > MAX_FW_SIZE) {
            out.print(F("ERR bad firmware size: "));
            out.println(fwSize);
            return false;
        }

        out.print(F("Firmware: 0x"));
        out.print(appStart, HEX);
        out.print(F(" size="));
        out.print(fwSize);
        out.println(F(" bytes"));

        // Compute CRC of internal flash firmware
        out.print(F("Computing internal flash CRC..."));
        uint16_t internalCrc = crc16((const uint8_t*)appStart, fwSize);
        out.print(F(" 0x"));
        out.println(internalCrc, HEX);

        // Erase QSPI staging area (header + firmware)
        uint32_t eraseBytes = FIRMWARE_ADDR + fwSize;
        uint32_t eraseSectors = (eraseBytes + SECTOR_SIZE - 1) / SECTOR_SIZE;
        out.print(F("Erasing QSPI ("));
        out.print(eraseSectors);
        out.print(F(" sectors)..."));

        for (uint32_t s = 0; s < eraseSectors; s++) {
            SafeBootWatchdog::feed();
            if (!eraseSector(s)) {
                out.println(F(" ERR erase failed"));
                return false;
            }
        }
        out.println(F(" OK"));

        // Copy firmware from internal flash to QSPI
        out.print(F("Writing to QSPI..."));
        uint8_t buf[256] __attribute__((aligned(4)));
        uint32_t remaining = fwSize;
        uint32_t srcAddr = appStart;
        uint32_t dstAddr = FIRMWARE_ADDR;

        while (remaining > 0) {
            SafeBootWatchdog::feed();
            uint32_t chunkLen = min(remaining, (uint32_t)sizeof(buf));
            memcpy(buf, (const void*)srcAddr, chunkLen);
            if (!qspiWrite(buf, chunkLen, dstAddr)) {
                out.println(F(" ERR write failed"));
                return false;
            }
            srcAddr += chunkLen;
            dstAddr += chunkLen;
            remaining -= chunkLen;
        }
        out.println(F(" OK"));

        // Read back from QSPI and verify CRC
        out.print(F("Verifying QSPI CRC..."));
        uint16_t qspiCrc = 0xFFFF;
        remaining = fwSize;
        dstAddr = FIRMWARE_ADDR;

        while (remaining > 0) {
            SafeBootWatchdog::feed();
            uint32_t chunkLen = min(remaining, (uint32_t)sizeof(buf));
            if (!qspiRead(buf, chunkLen, dstAddr)) {
                out.println(F(" ERR read failed"));
                return false;
            }
            qspiCrc = crc16(buf, chunkLen, qspiCrc);
            dstAddr += chunkLen;
            remaining -= chunkLen;
        }

        out.print(F(" 0x"));
        out.println(qspiCrc, HEX);

        if (qspiCrc != internalCrc) {
            out.print(F("ERR CRC mismatch! internal=0x"));
            out.print(internalCrc, HEX);
            out.print(F(" qspi=0x"));
            out.println(qspiCrc, HEX);
            return false;
        }

        out.println(F("QSPI self-test PASSED — CRC match"));

        // Write header WITHOUT magic — marks staging as "data present but not committed"
        // This lets ota status show the staged size/CRC without triggering bootloader apply
        StagingHeader hdr;
        hdr.size = fwSize;
        hdr.crc16 = internalCrc;
        hdr.magic = 0x0000;  // NOT HEADER_MAGIC — bootloader will ignore this
        if (!eraseSector(0)) {  // Must erase before writing header sector
            out.println(F("ERR sector 0 erase failed"));
            return false;
        }
        SafeBootWatchdog::feed();
        qspiWrite(&hdr, sizeof(hdr), HEADER_ADDR);

        out.print(F("Staged "));
        out.print(fwSize);
        out.print(F(" bytes, CRC=0x"));
        out.print(internalCrc, HEX);
        out.println(F(" (not committed — use 'ota commit' to apply)"));

        return true;
    }

    // =====================================================================
    // Signed OTA (docs/WEB_FLASHER.md). The only way to stage an image:
    //   ota manifest <304 hex>           signed manifest -> erase staging
    //   ota chunk <off> <crc16> <b64>    in-order chunk, <= MAX_CHUNK bytes
    //   ota commit                       read back + SHA-512 -> arm bootloader
    // Every reply is one line starting "OK ota" or "ERR ota <reason>".
    // =====================================================================

    static constexpr uint32_t MAX_CHUNK = 512;
    // CLAUDE.md 60-second rule: markStable() fires at 60 s of uptime; an
    // apply (a reset) before that counts toward the config quarantine.
    static constexpr uint32_t MIN_UPTIME_FOR_APPLY_MS = 75000;

    enum class State : uint8_t { IDLE, RECEIVING, COMPLETE };

    /** `ota manifest <hex>`: verify, then prepare the staging area. */
    bool beginSigned(const char* hex, Print& out) {
        reset();
        if (!ready_) {
            out.println(F("ERR ota qspi-not-ready"));
            return false;
        }
        uint8_t raw[ota::MANIFEST_LEN];
        if (!ota::hexDecode(hex, strlen(hex), raw, sizeof(raw))) {
            out.println(F("ERR ota bad-length"));
            return false;
        }
        ota::Manifest m;
        ota::Verdict v = ota::parseAndVerify(raw, sizeof(raw), ota::TRUSTED_PUBKEY,
                                             ota::BOARD_XIAO_NRF52840_SENSE, FIRMWARE_BUILD,
                                             MAX_FW_SIZE, m);
        if (v != ota::Verdict::OK) {
            out.print(F("ERR ota "));
            out.print(ota::verdictName(v));
            out.print(F(" current="));
            out.println((uint32_t)FIRMWARE_BUILD);
            return false;
        }

        // Erase header + image area. ~45 ms per 4 KB sector: a few seconds
        // for a full image, during which the main loop is blocked.
        uint32_t eraseSectors = (FIRMWARE_ADDR + m.size + SECTOR_SIZE - 1) / SECTOR_SIZE;
        for (uint32_t s = 0; s < eraseSectors; s++) {
            SafeBootWatchdog::feed();
            if (!eraseSector(s)) {
                out.println(F("ERR ota qspi-erase"));
                return false;
            }
        }
        manifest_ = m;
        crypto_sha512_init(&sha_);
        next_ = 0;
        state_ = State::RECEIVING;
        out.print(F("OK ota ready build="));
        out.print(m.build);
        out.print(F(" size="));
        out.print(m.size);
        out.print(F(" chunk="));
        out.println(MAX_CHUNK);
        return true;
    }

    /** `ota chunk <offset> <crc16hex> <base64>`: append one in-order chunk. */
    bool writeChunkSigned(uint32_t offset, uint16_t crc, const char* b64, Print& out) {
        if (state_ != State::RECEIVING) {
            out.println(F("ERR ota not-receiving"));
            return false;
        }
        // +3: room to pad a final odd-length chunk to the 4-byte QSPI unit.
        uint8_t buf[MAX_CHUNK + 3] __attribute__((aligned(4)));
        int len = base64Decode(b64, strlen(b64), buf, MAX_CHUNK);
        if (len <= 0) {
            out.println(F("ERR ota bad-chunk"));
            return false;
        }
        // A truncated or corrupted line (e.g. a BLE write cut at the MTU)
        // fails here; the host resends with a smaller write size.
        if (crc16(buf, (uint32_t)len) != crc) {
            out.println(F("ERR ota chunk-crc"));
            return false;
        }
        if (offset < next_ && offset + (uint32_t)len <= next_) {
            // Resend of a chunk whose OK was lost: already written, not re-hashed.
            printNext(out);
            return true;
        }
        if (offset != next_) {
            out.print(F("ERR ota offset expected="));
            out.println(next_);
            return false;
        }
        uint32_t end = offset + (uint32_t)len;
        if (end > manifest_.size) {
            out.println(F("ERR ota overflow"));
            return false;
        }
        if ((len & 3) != 0 && end != manifest_.size) {
            out.println(F("ERR ota unaligned-chunk"));
            return false;
        }
        for (int i = len; i < ((len + 3) & ~3); i++) buf[i] = 0xFF;  // erased-flash padding
        SafeBootWatchdog::feed();
        if (!writePaged(FIRMWARE_ADDR + offset, buf, (uint32_t)len)) {
            reset();
            out.println(F("ERR ota qspi-write"));
            return false;
        }
        crypto_sha512_update(&sha_, buf, (size_t)len);
        next_ = end;
        if (next_ < manifest_.size) {
            printNext(out);
            return true;
        }
        uint8_t digest[64];
        crypto_sha512_final(&sha_, digest);
        if (memcmp(digest, manifest_.sha512, sizeof(digest)) != 0) {
            reset();
            out.println(F("ERR ota hash-mismatch"));
            return false;
        }
        state_ = State::COMPLETE;
        out.print(F("OK ota complete size="));
        out.println(next_);
        return true;
    }

    /**
     * `ota commit`: re-read the staged image from QSPI and check its SHA-512
     * against the signed manifest (catches a QSPI write that silently
     * failed), then write the bootloader's header (size + CRC16 of what is
     * actually in QSPI) and reset into the bootloader's apply path.
     */
    bool commitSigned(Print& out) {
        if (state_ != State::COMPLETE) {
            out.println(F("ERR ota not-complete"));
            return false;
        }
        if (millis() < MIN_UPTIME_FOR_APPLY_MS) {
            out.print(F("ERR ota too-soon wait_ms="));
            out.println(MIN_UPTIME_FOR_APPLY_MS - millis());
            return false;
        }
        crypto_sha512_ctx sha;
        crypto_sha512_init(&sha);
        uint16_t crc = 0xFFFF;
        uint8_t buf[256] __attribute__((aligned(4)));
        for (uint32_t done = 0; done < manifest_.size;) {
            SafeBootWatchdog::feed();
            uint32_t n = min(manifest_.size - done, (uint32_t)sizeof(buf));
            if (!qspiRead(buf, n, FIRMWARE_ADDR + done)) {
                reset();
                out.println(F("ERR ota qspi-read"));
                return false;
            }
            crypto_sha512_update(&sha, buf, n);
            crc = crc16(buf, n, crc);
            done += n;
        }
        uint8_t digest[64];
        crypto_sha512_final(&sha, digest);
        if (memcmp(digest, manifest_.sha512, sizeof(digest)) != 0) {
            reset();
            out.println(F("ERR ota readback-mismatch"));
            return false;
        }

        StagingHeader hdr;
        hdr.size = manifest_.size;
        hdr.crc16 = crc;
        hdr.magic = HEADER_MAGIC;
        if (!eraseSector(0) || !qspiWrite(&hdr, sizeof(hdr), HEADER_ADDR)) {
            reset();
            out.println(F("ERR ota header-write"));
            return false;
        }
        StagingHeader check;
        if (!readHeader(check) || check.magic != HEADER_MAGIC || check.size != hdr.size ||
            check.crc16 != hdr.crc16) {
            eraseSector(0);
            reset();
            out.println(F("ERR ota header-verify"));
            return false;
        }

        out.print(F("OK ota applying build="));
        out.println(manifest_.build);
        delay(100);  // let the reply leave over USB CDC / BLE before the reset
        // Dual-path apply request, as SerialConsole's `bootloader` command:
        // RAM at 0x20007F7C survives the reset but a Windows USB hub
        // power-cycle can clear it; GPREGRET survives that. The bootloader
        // checks the RAM magic first, then GPREGRET (main.c check_dfu_mode).
        volatile uint32_t* bootloader_ram = (volatile uint32_t*)0x20007F7C;
        *bootloader_ram = 0xBEEF00CC;  // DFU_RAM_MAGIC_QSPI
        NRF_POWER->GPREGRET = GPREGRET_QSPI_APPLY;
        __DSB(); __ISB();
        NVIC_SystemReset();
        return true;  // not reached
    }

    /** `ota status`: one JSON line for hosts (the web updater). */
    void printStatusJson(Print& out, uint16_t bleMtu) {
        out.print(F("{\"ota\":{\"proto\":1,\"state\":\""));
        if (state_ == State::IDLE) out.print(F("idle"));
        else if (state_ == State::RECEIVING) out.print(F("receiving"));
        else out.print(F("complete"));
        out.print(F("\",\"next\":"));
        out.print(next_);
        out.print(F(",\"size\":"));
        out.print(state_ == State::IDLE ? 0 : manifest_.size);
        out.print(F(",\"build\":"));
        out.print(state_ == State::IDLE ? 0 : manifest_.build);
        out.print(F(",\"current\":"));
        out.print((uint32_t)FIRMWARE_BUILD);
        out.print(F(",\"key\":\""));
        out.print(ota::TRUSTED_KEY_NAME);
        out.print(F("\",\"maxChunk\":"));
        out.print(MAX_CHUNK);
        out.print(F(",\"mtu\":"));
        out.print(bleMtu);
        out.print(F(",\"qspi\":"));
        out.print(ready_ ? F("true") : F("false"));
        out.print(F(",\"qe\":"));
        out.print((sr2_ & 0x02) ? F("true") : F("false"));
        out.println(F("}}"));
    }

    /**
     * Clear the staging header (abort any pending OTA).
     */
    bool abort(Print& out) {
        reset();
        if (!ready_) {
            out.println(F("ERR ota qspi-not-ready"));
            return false;
        }
        if (!eraseSector(0)) {
            out.println(F("ERR ota qspi-erase"));
            return false;
        }
        SafeBootWatchdog::feed();
        out.println(F("OK ota aborted"));
        return true;
    }

private:
    bool ready_ = false;
    State state_ = State::IDLE;
    ota::Manifest manifest_;
    crypto_sha512_ctx sha_;
    uint32_t next_ = 0;

    uint8_t sr2_ = 0;

    void reset() {
        state_ = State::IDLE;
        next_ = 0;
    }

    /**
     * Custom instruction with IO2/IO3 driven HIGH. nrfx's default holds them
     * low, which on this flash asserts WP# / HOLD# and the chip ignores the
     * command. `len` counts the opcode byte.
     */
    static bool cinstr(uint8_t opcode, nrf_qspi_cinstr_len_t len, const void* tx, void* rx,
                       bool wren = false) {
        nrf_qspi_cinstr_conf_t c = NRFX_QSPI_DEFAULT_CINSTR(opcode, len);
        c.io2_level = true;
        c.io3_level = true;
        c.wren = wren;
        c.wipwait = wren;
        return nrfx_qspi_cinstr_xfer(&c, tx, rx) == NRFX_SUCCESS;
    }

    /**
     * The QSPI config reads and programs in quad mode (READ4O / PP4O), and so
     * does the bootloader when it applies a staged image. Quad mode only works
     * with the flash's non-volatile Quad Enable bit set (P25Q16H: status
     * register 2, bit 1), and it ships cleared: with QE=0 every staged image
     * read back differently from what was written (measured on hat_v1,
     * 2026-10-20: `ota selftest` CRC mismatch, `ota commit` readback-mismatch).
     * Set it once (it persists in the flash), then verify; fail loudly.
     */
    bool ensureQuadEnable() {
        uint8_t sr1 = 0, sr2 = 0;
        if (!cinstr(0x05, NRF_QSPI_CINSTR_LEN_2B, nullptr, &sr1) ||
            !cinstr(0x35, NRF_QSPI_CINSTR_LEN_2B, nullptr, &sr2)) {
            Serial.println(F("[ERROR] QSPI: cannot read the flash status registers"));
            return false;
        }
        if (!(sr2 & 0x02)) {
            // WRSR (01h) writes SR1 then SR2; SR1 is written back unchanged.
            uint8_t tx[2] = {sr1, (uint8_t)(sr2 | 0x02)};
            if (!cinstr(0x01, NRF_QSPI_CINSTR_LEN_3B, tx, nullptr, true)) {
                Serial.println(F("[ERROR] QSPI: write-status command failed"));
                return false;
            }
            // Status writes take up to ~15 ms; wipwait covers it, this is a backstop.
            for (int i = 0; i < 50 && nrfx_qspi_mem_busy_check() != NRFX_SUCCESS; i++) delay(1);
            uint8_t after = 0;
            if (!cinstr(0x35, NRF_QSPI_CINSTR_LEN_2B, nullptr, &after) || !(after & 0x02)) {
                Serial.print(F("[ERROR] QSPI: could not set Quad Enable (SR2 0x"));
                Serial.print(sr2, HEX);
                Serial.print(F(" -> 0x"));
                Serial.print(after, HEX);
                Serial.println(F(")"));
                return false;
            }
            Serial.print(F("[OTA] QSPI Quad Enable set (SR2 0x"));
            Serial.print(sr2, HEX);
            Serial.print(F(" -> 0x"));
            Serial.print(after, HEX);
            Serial.println(F(")"));
            sr2 = after;
        }
        sr2_ = sr2;
        return true;
    }

    void printNext(Print& out) {
        out.print(F("OK ota next="));
        out.println(next_);
    }

    /** QSPI page program is 256 bytes: never let one write cross a page. */
    bool writePaged(uint32_t addr, const uint8_t* data, uint32_t len) {
        uint32_t o = 0;
        while (o < len) {
            uint32_t n = min(len - o, (uint32_t)(256 - ((addr + o) & 0xFF)));
            if (!qspiWrite(data + o, n, addr + o)) return false;
            o += n;
        }
        return true;
    }

    /**
     * Minimal base64 decoder. Returns decoded length, or -1 on error.
     */
    static int base64Decode(const char* in, size_t inLen, uint8_t* out, size_t outMax) {
        size_t o = 0;
        uint32_t accum = 0;
        int bits = 0;

        for (size_t i = 0; i < inLen; i++) {
            char c = in[i];
            uint8_t val;
            if (c >= 'A' && c <= 'Z')      val = c - 'A';
            else if (c >= 'a' && c <= 'z')  val = c - 'a' + 26;
            else if (c >= '0' && c <= '9')  val = c - '0' + 52;
            else if (c == '+')              val = 62;
            else if (c == '/')              val = 63;
            else if (c == '=')              break;  // Padding
            else                            continue;  // Skip whitespace/invalid

            accum = (accum << 6) | val;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                if (o >= outMax) return -1;
                out[o++] = (uint8_t)(accum >> bits);
                accum &= (1 << bits) - 1;
            }
        }
        return (int)o;
    }
};

#else

// Stub for non-nRF52 platforms
// cppcheck-suppress ctuOneDefinitionRuleViolation
class QspiOtaStaging {
public:
    bool begin() { return false; }
    bool isReady() const { return false; }
    void printStatusJson(Print& out, uint16_t) { out.println(F("ERR ota not-supported")); }
    bool selfTest(Print& out) { out.println(F("ERR ota not-supported")); return false; }
    bool beginSigned(const char*, Print& out) { out.println(F("ERR ota not-supported")); return false; }
    bool writeChunkSigned(uint32_t, uint16_t, const char*, Print& out) { out.println(F("ERR ota not-supported")); return false; }
    bool commitSigned(Print& out) { out.println(F("ERR ota not-supported")); return false; }
    bool abort(Print& out) { out.println(F("ERR ota not-supported")); return false; }
};

#endif
