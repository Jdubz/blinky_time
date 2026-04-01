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
 * Serial/BLE commands:
 *   ota selftest   — Copy current firmware from internal flash to QSPI staging,
 *                     read back and verify CRC. Non-destructive test.
 *   ota commit     — Validate staged firmware CRC, write header, set GPREGRET=0xCC,
 *                     reset. Bootloader applies the staged image.
 *   ota status     — Report staging area state (size, CRC, valid).
 *   ota abort      — Clear staging header.
 */

#ifdef ARDUINO_ARCH_NRF52

#include <Arduino.h>
#include "../hal/SafeBootWatchdog.h"

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
        ready_ = true;
        return true;
    }

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
     * Report staging area status to output.
     */
    void printStatus(Print& out) {
        if (!ready_) {
            out.println(F("QSPI not initialized"));
            return;
        }
        out.println(F("QSPI flash: P25Q16H 2 MB"));

        StagingHeader hdr;
        if (!readHeader(hdr)) {
            out.println(F("QSPI header read failed"));
            return;
        }

        out.print(F("Staging: magic=0x"));
        out.print(hdr.magic, HEX);
        out.print(F(" size="));
        out.print(hdr.size);
        out.print(F(" crc=0x"));
        out.println(hdr.crc16, HEX);

        if (hdr.magic == HEADER_MAGIC && hdr.size > 0 && hdr.size <= MAX_FW_SIZE) {
            out.println(F("Staging: VALID header — ready to commit"));
        } else {
            out.println(F("Staging: no valid firmware staged"));
        }
    }

    /**
     * Self-test: copy current firmware from internal flash to QSPI staging,
     * read back and verify CRC. Non-destructive (doesn't write the header
     * magic, so bootloader won't apply it).
     *
     * This tests the full QSPI write/read/CRC pipeline.
     */
    bool selfTest(Print& out) {
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
        eraseSector(0);  // Must erase before writing header sector
        SafeBootWatchdog::feed();
        qspiWrite(&hdr, sizeof(hdr), HEADER_ADDR);

        out.print(F("Staged "));
        out.print(fwSize);
        out.print(F(" bytes, CRC=0x"));
        out.print(internalCrc, HEX);
        out.println(F(" (not committed — use 'ota commit' to apply)"));

        return true;
    }

    /**
     * Commit the staged firmware: validate CRC, write header magic,
     * set GPREGRET=0xCC, and reset. The bootloader will apply the
     * staged image on next boot.
     *
     * THIS IS THE POINT OF NO RETURN for the current boot — the device
     * will reset. If the staged firmware is valid, the bootloader applies
     * it. If invalid (CRC mismatch), the bootloader skips QSPI and boots
     * the old app.
     */
    bool commit(Print& out) {
        if (!ready_) {
            out.println(F("ERR QSPI not initialized"));
            return false;
        }

        // Read current header
        StagingHeader hdr;
        if (!readHeader(hdr)) {
            out.println(F("ERR cannot read staging header"));
            return false;
        }

        if (hdr.size == 0 || hdr.size > MAX_FW_SIZE) {
            out.println(F("ERR no valid firmware staged"));
            return false;
        }

        // Verify CRC of staged firmware before committing
        out.print(F("Verifying staged firmware CRC..."));
        uint8_t buf[256] __attribute__((aligned(4)));
        uint16_t computedCrc = 0xFFFF;
        uint32_t remaining = hdr.size;
        uint32_t addr = FIRMWARE_ADDR;

        while (remaining > 0) {
            SafeBootWatchdog::feed();
            uint32_t chunkLen = min(remaining, (uint32_t)sizeof(buf));
            if (!qspiRead(buf, chunkLen, addr)) {
                out.println(F(" ERR read failed"));
                return false;
            }
            computedCrc = crc16(buf, chunkLen, computedCrc);
            addr += chunkLen;
            remaining -= chunkLen;
        }

        if (computedCrc != hdr.crc16) {
            out.print(F(" ERR CRC mismatch: header=0x"));
            out.print(hdr.crc16, HEX);
            out.print(F(" computed=0x"));
            out.println(computedCrc, HEX);
            return false;
        }
        out.println(F(" OK"));

        // Write header with valid magic — this arms the bootloader
        out.println(F("Writing commit header..."));
        eraseSector(0);
        SafeBootWatchdog::feed();
        hdr.magic = HEADER_MAGIC;
        qspiWrite(&hdr, sizeof(hdr), HEADER_ADDR);

        // Verify header was written correctly
        StagingHeader verifyHdr;
        qspiRead(&verifyHdr, sizeof(verifyHdr), HEADER_ADDR);
        if (verifyHdr.magic != HEADER_MAGIC || verifyHdr.size != hdr.size ||
            verifyHdr.crc16 != hdr.crc16) {
            out.println(F("ERR header verify failed — aborting commit"));
            // Clear the bad header
            eraseSector(0);
            return false;
        }

        out.print(F("Committing: size="));
        out.print(hdr.size);
        out.print(F(" crc=0x"));
        out.println(hdr.crc16, HEX);

        // Set GPREGRET=0xCC and reset
        out.println(F("Setting GPREGRET=0xCC and resetting..."));
        Serial.flush();
        delay(100);

        uint8_t sd_en = 0;
        sd_softdevice_is_enabled(&sd_en);
        if (sd_en) {
            sd_power_gpregret_clr(0, 0xFF);
            sd_power_gpregret_set(0, GPREGRET_QSPI_APPLY);
        } else {
            NRF_POWER->GPREGRET = GPREGRET_QSPI_APPLY;
        }
        __DSB(); __ISB();
        NVIC_SystemReset();

        return true;  // Never reached
    }

    /**
     * Begin a new OTA transfer: erase QSPI staging area and record expected
     * size + CRC. Called by server before sending chunks.
     */
    bool beginTransfer(uint32_t size, uint16_t expectedCrc, Print& out) {
        if (!ready_) {
            out.println(F("ERR QSPI not initialized"));
            return false;
        }
        if (size == 0 || size > MAX_FW_SIZE) {
            out.print(F("ERR invalid size: "));
            out.println(size);
            return false;
        }

        // Erase staging area (header + firmware region)
        uint32_t eraseBytes = FIRMWARE_ADDR + size;
        uint32_t eraseSectors = (eraseBytes + SECTOR_SIZE - 1) / SECTOR_SIZE;
        for (uint32_t s = 0; s < eraseSectors; s++) {
            SafeBootWatchdog::feed();
            if (!eraseSector(s)) {
                out.println(F("ERR erase failed"));
                return false;
            }
        }

        // Write header WITHOUT magic (uncommitted)
        StagingHeader hdr;
        hdr.size = size;
        hdr.crc16 = expectedCrc;
        hdr.magic = 0x0000;
        qspiWrite(&hdr, sizeof(hdr), HEADER_ADDR);

        transferSize_ = size;
        transferCrc_ = expectedCrc;
        transferActive_ = true;

        out.print(F("OK ota begin size="));
        out.print(size);
        out.print(F(" crc=0x"));
        out.println(expectedCrc, HEX);
        return true;
    }

    /**
     * Write a chunk of firmware data to QSPI at the given offset.
     * Data is base64-encoded in the command string.
     */
    bool writeChunk(uint32_t offset, const char* base64Data, Print& out) {
        if (!ready_ || !transferActive_) {
            out.println(F("ERR no active transfer"));
            return false;
        }

        // Decode base64
        size_t b64Len = strlen(base64Data);
        // Base64 output is at most 3/4 of input length
        size_t maxDecoded = (b64Len * 3) / 4 + 4;
        if (maxDecoded > 256) {
            out.println(F("ERR chunk too large"));
            return false;
        }

        uint8_t decoded[256] __attribute__((aligned(4)));
        int decodedLen = base64Decode(base64Data, b64Len, decoded, sizeof(decoded));
        if (decodedLen <= 0) {
            out.println(F("ERR base64 decode failed"));
            return false;
        }

        if (offset + decodedLen > transferSize_) {
            out.println(F("ERR chunk exceeds firmware size"));
            return false;
        }

        SafeBootWatchdog::feed();
        if (!qspiWrite(decoded, decodedLen, FIRMWARE_ADDR + offset)) {
            out.println(F("ERR QSPI write failed"));
            return false;
        }

        out.print(F("OK ota chunk offset="));
        out.print(offset);
        out.print(F(" len="));
        out.println(decodedLen);
        return true;
    }

    /**
     * Clear the staging header (abort any pending OTA).
     */
    bool abort(Print& out) {
        if (!ready_) {
            out.println(F("ERR QSPI not initialized"));
            return false;
        }
        eraseSector(0);
        SafeBootWatchdog::feed();
        out.println(F("OK staging cleared"));
        return true;
    }

private:
    bool ready_ = false;
    bool transferActive_ = false;
    uint32_t transferSize_ = 0;
    uint16_t transferCrc_ = 0;

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
    void printStatus(Print& out) { out.println(F("QSPI OTA not available on this platform")); }
    bool selfTest(Print& out) { out.println(F("QSPI OTA not available")); return false; }
    bool commit(Print& out) { out.println(F("QSPI OTA not available")); return false; }
    bool abort(Print& out) { out.println(F("QSPI OTA not available")); return false; }
};

#endif
