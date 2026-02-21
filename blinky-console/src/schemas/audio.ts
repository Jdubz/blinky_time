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
 * Music mode data from streaming `{"m":{...}}` messages
 *
 * Sent by AudioController with CBSS beat tracking.
 */
export const MusicModeDataSchema = z.object({
  a: z.union([z.literal(0), z.literal(1)]), // Active flag (rhythm detected)
  bpm: z.number().nonnegative(), // Tempo in BPM (0 when inactive)
  ph: z.number().min(0).max(1), // Phase (0-1, 0=on-beat)
  str: z.number().min(0).max(1), // Rhythm strength (0-1)
  conf: z.number().min(0).max(1), // CBSS beat tracking confidence (0-1)
  bc: z.number().int().nonnegative(), // Beat count (tracked beats)
  q: z.union([z.literal(0), z.literal(1)]), // Beat event (phase wrap)
  e: z.number().min(0).max(1), // Energy (0-1)
  p: z.number().min(0).max(1), // Pulse (0-1)
  cb: z.number().nonnegative().optional(), // Current CBSS value
  oss: z.number().nonnegative().optional(), // Smoothed onset strength
  ttb: z.number().int().optional(), // Frames until next beat
  bp: z.union([z.literal(0), z.literal(1)]).optional(), // Last beat predicted (1) vs fallback (0)
});

export type MusicModeData = z.infer<typeof MusicModeDataSchema>;

/**
 * Streaming audio message format
 */
export const AudioMessageSchema = z.object({
  a: AudioSampleSchema,
  m: MusicModeDataSchema.optional(), // Optional music mode data
});

export type AudioMessage = z.infer<typeof AudioMessageSchema>;
