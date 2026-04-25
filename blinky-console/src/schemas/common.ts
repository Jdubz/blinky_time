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
export const GeneratorTypeSchema = z.enum(['fire', 'water', 'lightning', 'audio']);

export type GeneratorType = z.infer<typeof GeneratorTypeSchema>;

/**
 * Effect types supported by RenderPipeline
 */
export const EffectTypeSchema = z.enum(['none', 'hue']);

export type EffectType = z.infer<typeof EffectTypeSchema>;
