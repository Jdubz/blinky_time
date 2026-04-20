// Native parity harness for SharedSpectralAnalysis shape features.
//
// Reads a binary file of pre-computed magnitude frames (produced by the
// Python wrapper: `ml-training/analysis/run_parity_test.py`), injects each
// frame into the firmware's `preWhitenMagnitudes_` buffer, runs
// `computeShapeFeaturesRaw()`, and writes per-frame feature values to CSV.
//
// Binary input format (little-endian, host-byte-order float32):
//   int32   NUM_BINS
//   int32   N_FRAMES
//   float32 mags[N_FRAMES * NUM_BINS]
//
// The int32 header lets us catch mismatched NUM_BINS at parse time instead
// of producing silently wrong output. Magnitudes are in column-major order
// per frame: frame 0 bins 0..N-1, frame 1 bins 0..N-1, etc.
//
// CSV output: one header row + one row per frame with
//   frame,centroid,crest,rolloff,hfc

#include "audio/SharedSpectralAnalysis.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

static int usage() {
    std::fprintf(stderr,
        "Usage: parity_harness <mags.bin> <out.csv>\n"
        "\n"
        "See tests/parity/README.md for the binary format.\n");
    return 2;
}

int main(int argc, char** argv) {
    if (argc != 3) return usage();
    const char* mags_path = argv[1];
    const char* csv_path = argv[2];

    std::ifstream in(mags_path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "parity_harness: cannot open %s\n", mags_path);
        return 1;
    }
    int32_t num_bins = 0;
    int32_t n_frames = 0;
    in.read(reinterpret_cast<char*>(&num_bins), sizeof(num_bins));
    in.read(reinterpret_cast<char*>(&n_frames), sizeof(n_frames));
    if (!in) {
        std::fprintf(stderr, "parity_harness: short read on header\n");
        return 1;
    }
    if (num_bins != SpectralConstants::NUM_BINS) {
        std::fprintf(stderr,
            "parity_harness: NUM_BINS mismatch — input has %d, firmware has %d\n",
            num_bins, SpectralConstants::NUM_BINS);
        return 1;
    }
    if (n_frames <= 0) {
        std::fprintf(stderr, "parity_harness: n_frames=%d, nothing to do\n", n_frames);
        return 1;
    }

    // Load the full magnitude buffer into memory. For the corpus-sized runs
    // (≈100k frames × 128 bins × 4 B = ~50 MB) this is comfortably fine.
    std::vector<float> mags(static_cast<size_t>(n_frames) * num_bins);
    in.read(reinterpret_cast<char*>(mags.data()),
            static_cast<std::streamsize>(mags.size() * sizeof(float)));
    if (!in) {
        std::fprintf(stderr, "parity_harness: short read on body\n");
        return 1;
    }

    SharedSpectralAnalysis spectral;

    std::ofstream out(csv_path);
    if (!out) {
        std::fprintf(stderr, "parity_harness: cannot open %s for write\n", csv_path);
        return 1;
    }
    out << "frame,centroid,crest,rolloff,hfc\n";
    // Use 9 decimals so float32 is round-tripped exactly — anything less
    // would silently limit parity resolution.
    out.precision(9);

    for (int32_t f = 0; f < n_frames; f++) {
        const float* frame_mags = mags.data() + static_cast<size_t>(f) * num_bins;
        spectral.setPreWhitenMagnitudesForTest(frame_mags);
        spectral.runShapeFeaturesForTest();
        out << f << ','
            << spectral.getRawCentroid() << ','
            << spectral.getRawCrest() << ','
            << spectral.getRawRolloff() << ','
            << spectral.getRawHFC() << '\n';
    }

    std::fprintf(stderr, "parity_harness: wrote %d frames to %s\n", n_frames, csv_path);
    return 0;
}
