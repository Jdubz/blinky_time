/**
 * Event message Zod schemas
 *
 * Validates transient, rhythm, and status event messages from the device.
 */

import { z } from 'zod';

/**
 * Detection modes for transient detection
 */
export const DetectionModeSchema = z.union([
  z.literal(0), // drummer
  z.literal(1), // bass
  z.literal(2), // HFC
  z.literal(3), // flux
  z.literal(4), // hybrid
]);

export type DetectionMode = z.infer<typeof DetectionModeSchema>;

/**
 * Transient detection message from `{"type":"TRANSIENT",...}` messages
 *
 * Firmware sends minimal format: {"type":"TRANSIENT","timestampMs":123,"strength":0.85}
 * Extended fields (mode, level, energy, ts) are optional for future expansion.
 */
export const TransientMessageSchema = z.object({
  type: z.literal('TRANSIENT'),
  timestampMs: z.number().int().nonnegative(), // Primary timestamp field
  strength: z.number().min(0).max(1), // Transient strength (0-1)
  // Optional extended fields (not currently sent by firmware)
  ts: z.number().int().nonnegative().optional(), // Alias for timestampMs
  mode: DetectionModeSchema.optional(), // Detection mode (0-4)
  level: z.number().min(0).max(1).optional(), // Normalized level (0-1)
  energy: z.number().min(0).optional(), // Mode-specific energy value
});

export type TransientMessage = z.infer<typeof TransientMessageSchema>;

/**
 * Rhythm analyzer telemetry from `{"type":"RHYTHM",...}` messages
 */
export const RhythmMessageSchema = z.object({
  type: z.literal('RHYTHM'),
  bpm: z.number().positive(), // Detected BPM
  strength: z.number().min(0).max(1), // Periodicity strength (0-1)
  periodMs: z.number().positive(), // Detected period in milliseconds
  likelihood: z.number().min(0).max(1), // Beat likelihood (0-1)
  phase: z.number().min(0).max(1), // Current phase (0-1)
  bufferFill: z.number().int().min(0).max(256), // Buffer fill level
});

export type RhythmMessage = z.infer<typeof RhythmMessageSchema>;

/**
 * System status telemetry from `{"type":"STATUS",...}` messages
 */
export const StatusMessageSchema = z.object({
  type: z.literal('STATUS'),
  ts: z.number().int().nonnegative(), // Timestamp in milliseconds
  mode: DetectionModeSchema, // Detection mode (0-4)
  hwGain: z.number().int().min(0).max(80), // Hardware gain (0-80)
  level: z.number().min(0).max(1), // Current level (0-1)
  avgLevel: z.number().min(0).max(1), // Recent average level (0-1)
  peakLevel: z.number().min(0).max(1), // Peak level (0-1)
});

export type StatusMessage = z.infer<typeof StatusMessageSchema>;
