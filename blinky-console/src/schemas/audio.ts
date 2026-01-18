/**
 * Audio streaming Zod schemas
 *
 * Validates streaming audio messages `{"a":{...}}` from the device.
 */

import { z } from 'zod';

/**
 * Audio sample from streaming `{"a":{...}}` messages
 */
export const AudioSampleSchema = z.object({
  l: z.number().min(0).max(1), // level (0-1, post-range-mapping output)
  t: z.number().min(0).max(1), // transient strength (0-1)
  pk: z.number().min(0).max(1), // peak level (0-1)
  vl: z.number().min(0).max(1), // valley level (0-1)
  raw: z.number().min(0).max(1), // raw ADC level (0-1)
  h: z.number().int().min(0).max(80), // hardware gain (0-80)
  alive: z.union([z.literal(0), z.literal(1)]), // PDM status
  z: z.number().min(0).max(1).optional(), // zero-crossing rate (0-1) - optional, not always sent
});

export type AudioSample = z.infer<typeof AudioSampleSchema>;

/**
 * Rhythm analyzer data from streaming `{"r":{...}}` messages
 */
export const RhythmDataSchema = z.object({
  bpm: z.number().positive(), // Detected BPM
  str: z.number().min(0).max(1), // Periodicity strength (0-1)
  per: z.number().positive(), // Detected period in milliseconds
  lik: z.number().min(0).max(1), // Beat likelihood (0-1)
  ph: z.number().min(0).max(1), // Phase (0-1)
  buf: z.number().int().min(0).max(256), // Buffer fill level
});

export type RhythmData = z.infer<typeof RhythmDataSchema>;

/**
 * Music mode data from streaming `{"m":{...}}` messages
 */
export const MusicModeDataSchema = z.object({
  a: z.union([z.literal(0), z.literal(1)]), // Active flag
  bpm: z.number().positive(), // Tempo in BPM
  ph: z.number().min(0).max(1), // Phase (0-1)
  conf: z.number().min(0).max(1), // Confidence (0-1)
  q: z.union([z.literal(0), z.literal(1)]), // Quarter note event
  h: z.union([z.literal(0), z.literal(1)]), // Half note event
  w: z.union([z.literal(0), z.literal(1)]), // Whole note event
});

export type MusicModeData = z.infer<typeof MusicModeDataSchema>;

/**
 * Streaming audio message format
 */
export const AudioMessageSchema = z.object({
  a: AudioSampleSchema,
  r: RhythmDataSchema.optional(), // Optional rhythm data
  m: MusicModeDataSchema.optional(), // Optional music mode data
});

export type AudioMessage = z.infer<typeof AudioMessageSchema>;
