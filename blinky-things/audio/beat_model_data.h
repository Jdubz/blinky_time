// Placeholder — replaced by ml-training/scripts/export_tflite.py after training
// This file allows compilation without a trained model.
// When ENABLE_NN_BEAT_ACTIVATION is defined, a real model must be exported here.
// The static_assert in BeatActivationNN.h will catch this at compile time.

#ifndef BEAT_MODEL_DATA_H
#define BEAT_MODEL_DATA_H

#define BEAT_MODEL_HASH "placeholder"
#define BEAT_MODEL_SIZE 4

// Empty placeholder — BeatActivationNN::begin() will fail gracefully
// (model version check will return false)
alignas(8) const unsigned char beat_model_data[] = {
  0x00, 0x00, 0x00, 0x00,
};

const unsigned int beat_model_data_len = 4;

#endif // BEAT_MODEL_DATA_H
