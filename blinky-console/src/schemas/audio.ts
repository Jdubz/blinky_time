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
 * Music mode data from streaming `{"m":{...}}` sub-message.
 *
 * Field set tracks SerialConsole.cpp::streamTick() (PLP architecture, b79+).
 * Only `a`, `bpm`, `ph`, `str`, `q`, `e`, `p` are guaranteed every frame; the
 * rest are firmware-build-dependent so they're optional here.
 */
export const MusicModeDataSchema = z.object({
  a: z.union([z.literal(0), z.literal(1)]), // Active flag (rhythm detected)
  bpm: z.number().nonnegative(), // Tempo in BPM (0 when inactive)
  ph: z.number().min(0).max(1), // Phase (0-1, 0=on-beat)
  str: z.number().min(0).max(1), // Rhythm strength (0-1)
  q: z.union([z.literal(0), z.literal(1)]), // Beat event (phase wrap)
  e: z.number().min(0).max(1), // Energy (0-1)
  p: z.number().min(0).max(1), // Pulse (0-1)
  // Firmware-emitted, optional — not all builds include them.
  ts: z.number().nonnegative().optional(), // Firmware millis at frame emission
  pp: z.number().min(0).max(1).optional(), // PLP extracted pulse value
  od: z.number().nonnegative().optional(), // Onset density
  nn: z.number().min(0).max(1).optional(), // Raw NN onset activation
  per: z.number().int().nonnegative().optional(), // ACF period in analysis frames
  conf: z.number().min(0).max(1).optional(), // CBSS confidence (legacy field, pre-PLP)
  bc: z.number().int().nonnegative().optional(), // Beat count (legacy field, pre-PLP)
  cb: z.number().nonnegative().optional(), // Current CBSS value
  oss: z.number().nonnegative().optional(), // Smoothed onset strength
  ttb: z.number().int().optional(), // Frames until next beat
  bp: z.union([z.literal(0), z.literal(1)]).optional(), // Last beat predicted (1) vs fallback (0)
  bs: z.number().min(0).max(1).optional(), // Beat stability (b150+, debug-mode only). Rolling stability of inter-beat intervals.
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
