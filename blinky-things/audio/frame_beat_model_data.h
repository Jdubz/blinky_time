#pragma once

// Placeholder for frame-level beat activation FC model (INT8 TFLite flatbuffer).
// Replace with real model data exported by ml-training/scripts/export_frame_beat.py.
// The FrameBeatNN begin() check (len < 100) will detect this stub and skip init.

alignas(16) static const unsigned char frame_beat_model_data[] = {
    0x00, 0x00, 0x00, 0x00
};
static const unsigned int frame_beat_model_data_len = 4;
