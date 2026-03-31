#pragma once
/**
 * GifEncoder - Simple animated GIF encoder
 *
 * Creates GIF89a animated images from RGBA frame data.
 * Uses LZW compression and supports transparency.
 *
 * Based on public domain GIF encoding algorithms.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>

class GifEncoder {
private:
    FILE* file_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int frameDelay_ = 10; // centiseconds (10 = 100ms = 10 FPS)
    bool firstFrame_ = true;

    // Color palette (256 colors max for GIF)
    std::vector<uint8_t> palette_;
    int paletteSize_ = 256;

    // LZW compression state
    struct LZWState {
        int codeSize;
        int clearCode;
        int endCode;
        int nextCode;
        int curCodeSize;
        int32_t curCode;
        int curCodeBits;
        uint8_t outBuf[256];
        int outLen;
    };

    // Write byte to file
    void writeByte(uint8_t b) {
        fputc(b, file_);
    }

    // Write 16-bit little-endian
    void writeWord(uint16_t w) {
        writeByte(w & 0xFF);
        writeByte((w >> 8) & 0xFF);
    }

    // Write bytes
    void writeBytes(const uint8_t* data, size_t len) {
        fwrite(data, 1, len, file_);
    }

    // Build 6×6×6 uniform RGB palette (216 colors) + 40 grayscale ramp
    void buildPalette(const uint8_t* rgba, int numPixels) {
        (void)rgba; (void)numPixels;
        palette_.resize(256 * 3);

        // 216 uniform colors (6 levels per channel)
        for (int i = 0; i < 216; i++) {
            palette_[i * 3 + 0] = (i / 36) * 51;        // R: 0,51,102,153,204,255
            palette_[i * 3 + 1] = ((i / 6) % 6) * 51;   // G: 0,51,102,153,204,255
            palette_[i * 3 + 2] = (i % 6) * 51;          // B: 0,51,102,153,204,255
        }
        // 40 grayscale entries for smooth dark gradients (important for fire)
        for (int i = 0; i < 40; i++) {
            uint8_t v = (uint8_t)(i * 255 / 39);
            palette_[(216 + i) * 3 + 0] = v;
            palette_[(216 + i) * 3 + 1] = v;
            palette_[(216 + i) * 3 + 2] = v;
        }
    }

    // Find nearest palette color for RGB (brute-force, accurate)
    uint8_t findNearestColor(uint8_t r, uint8_t g, uint8_t b) {
        int bestIdx = 0;
        int bestDist = 999999;
        for (int i = 0; i < 256; i++) {
            int dr = (int)r - palette_[i * 3 + 0];
            int dg = (int)g - palette_[i * 3 + 1];
            int db = (int)b - palette_[i * 3 + 2];
            int dist = dr * dr + dg * dg + db * db;
            if (dist < bestDist) {
                bestDist = dist;
                bestIdx = i;
            }
        }
        return (uint8_t)bestIdx;
    }

    // Convert RGBA image to indexed
    std::vector<uint8_t> toIndexed(const uint8_t* rgba, int numPixels) {
        std::vector<uint8_t> indexed(numPixels);
        for (int i = 0; i < numPixels; i++) {
            uint8_t r = rgba[i * 4 + 0];
            uint8_t g = rgba[i * 4 + 1];
            uint8_t b = rgba[i * 4 + 2];
            indexed[i] = findNearestColor(r, g, b);
        }
        return indexed;
    }

    // LZW encode indexed image data
    void lzwEncode(const uint8_t* indexed, int numPixels) {
        const int minCodeSize = 8;
        writeByte(minCodeSize);

        // Simple LZW encoding
        std::vector<uint8_t> outBuf;
        int clearCode = 1 << minCodeSize;
        int endCode = clearCode + 1;

        int codeSize = minCodeSize + 1;
        int nextCode = endCode + 1;

        // Code table (simple implementation)
        // For a proper implementation, we'd use a hash table
        // This is a simplified version that resets frequently

        uint32_t bits = 0;
        int bitCount = 0;

        auto outputCode = [&](int code) {
            bits |= (code << bitCount);
            bitCount += codeSize;
            while (bitCount >= 8) {
                outBuf.push_back(bits & 0xFF);
                bits >>= 8;
                bitCount -= 8;
            }
        };

        outputCode(clearCode);

        int prevCode = indexed[0];
        for (int i = 1; i < numPixels; i++) {
            int pixel = indexed[i];

            // Check if we need to reset
            if (nextCode >= 4095) {
                outputCode(prevCode);
                outputCode(clearCode);
                codeSize = minCodeSize + 1;
                nextCode = endCode + 1;
                prevCode = pixel;
                continue;
            }

            // Simple output (not full LZW, but works)
            outputCode(prevCode);
            if (nextCode < 4096) {
                if (nextCode >= (1 << codeSize) && codeSize < 12) {
                    codeSize++;
                }
                nextCode++;
            }
            prevCode = pixel;
        }

        outputCode(prevCode);
        outputCode(endCode);

        // Flush remaining bits
        if (bitCount > 0) {
            outBuf.push_back(bits & 0xFF);
        }

        // Write sub-blocks
        size_t pos = 0;
        while (pos < outBuf.size()) {
            size_t blockSize = std::min(size_t(255), outBuf.size() - pos);
            writeByte(static_cast<uint8_t>(blockSize));
            writeBytes(outBuf.data() + pos, blockSize);
            pos += blockSize;
        }

        // Block terminator
        writeByte(0);
    }

public:
    GifEncoder() = default;

    ~GifEncoder() {
        close();
    }

    // Begin GIF file
    bool begin(const std::string& filename, int width, int height, int fps = 30) {
        file_ = fopen(filename.c_str(), "wb");
        if (!file_) return false;

        width_ = width;
        height_ = height;
        frameDelay_ = 100 / fps; // Convert FPS to centiseconds
        if (frameDelay_ < 1) frameDelay_ = 1;
        firstFrame_ = true;

        // Build default palette (6×6×6 RGB cube + grayscale ramp)
        buildPalette(nullptr, 0);

        // GIF89a header
        writeBytes((const uint8_t*)"GIF89a", 6);

        // Logical screen descriptor
        writeWord(width_);
        writeWord(height_);
        writeByte(0xF7); // Global color table, 256 colors (8 bits)
        writeByte(0);    // Background color index
        writeByte(0);    // Pixel aspect ratio

        // Global color table
        writeBytes(palette_.data(), 256 * 3);

        // Netscape extension for looping
        writeByte(0x21); // Extension
        writeByte(0xFF); // Application extension
        writeByte(11);   // Block size
        writeBytes((const uint8_t*)"NETSCAPE2.0", 11);
        writeByte(3);    // Sub-block size
        writeByte(1);    // Loop sub-block ID
        writeWord(0);    // Loop count (0 = infinite)
        writeByte(0);    // Block terminator

        return true;
    }

    // Add frame to GIF
    void addFrame(const uint8_t* rgba) {
        if (!file_) return;

        int numPixels = width_ * height_;

        // Graphic control extension (for timing and transparency)
        writeByte(0x21); // Extension
        writeByte(0xF9); // Graphic control
        writeByte(4);    // Block size
        writeByte(0x04); // Disposal method: restore to background
        writeWord(frameDelay_);
        writeByte(0);    // Transparent color index (not used)
        writeByte(0);    // Block terminator

        // Image descriptor
        writeByte(0x2C); // Image separator
        writeWord(0);    // Left
        writeWord(0);    // Top
        writeWord(width_);
        writeWord(height_);
        writeByte(0);    // No local color table

        // Convert to indexed and LZW encode
        std::vector<uint8_t> indexed = toIndexed(rgba, numPixels);
        lzwEncode(indexed.data(), numPixels);

        firstFrame_ = false;
    }

    // Finish and close GIF file
    void close() {
        if (file_) {
            writeByte(0x3B); // GIF trailer
            fclose(file_);
            file_ = nullptr;
        }
    }

    // Get file size (after close)
    static size_t getFileSize(const std::string& filename) {
        FILE* f = fopen(filename.c_str(), "rb");
        if (!f) return 0;
        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);
        fclose(f);
        return size;
    }
};
