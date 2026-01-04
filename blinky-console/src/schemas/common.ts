/**
 * Common types and schemas
 *
 * Types used across multiple schemas and components.
 */

import { z } from 'zod';

/**
 * Connection state
 */
export const ConnectionStateSchema = z.enum(['disconnected', 'connecting', 'connected', 'error']);

export type ConnectionState = z.infer<typeof ConnectionStateSchema>;

/**
 * Generator types supported by RenderPipeline
 */
export const GeneratorTypeSchema = z.enum(['fire', 'water', 'lightning']);

export type GeneratorType = z.infer<typeof GeneratorTypeSchema>;

/**
 * Effect types supported by RenderPipeline
 */
export const EffectTypeSchema = z.enum(['none', 'hue']);

export type EffectType = z.infer<typeof EffectTypeSchema>;

/**
 * Generator state
 */
export const GeneratorStateSchema = z.object({
  current: GeneratorTypeSchema,
  available: z.array(GeneratorTypeSchema),
});

export type GeneratorState = z.infer<typeof GeneratorStateSchema>;

/**
 * Effect state
 */
export const EffectStateSchema = z.object({
  current: EffectTypeSchema,
  available: z.array(EffectTypeSchema),
});

export type EffectState = z.infer<typeof EffectStateSchema>;
