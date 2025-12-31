/**
 * Settings Zod schemas
 *
 * Validates responses from the `json settings` command.
 */

import { z } from 'zod';

/**
 * Setting types supported by firmware
 */
export const SettingTypeSchema = z.enum(['uint8', 'int8', 'uint16', 'uint32', 'float', 'bool']);

export type SettingType = z.infer<typeof SettingTypeSchema>;

/**
 * Individual setting from `json settings` command
 */
export const DeviceSettingSchema = z.object({
  name: z.string().min(1, 'Setting name is required'),
  value: z.union([z.number(), z.boolean()]),
  type: SettingTypeSchema,
  cat: z.string().min(1, 'Category is required'),
  min: z.number(),
  max: z.number(),
  desc: z.string().optional(),
});

export type DeviceSetting = z.infer<typeof DeviceSettingSchema>;

/**
 * Response from `json settings` command
 */
export const SettingsResponseSchema = z.object({
  settings: z.array(DeviceSettingSchema),
});

export type SettingsResponse = z.infer<typeof SettingsResponseSchema>;

/**
 * Settings grouped by category (derived type, not from firmware)
 */
export const SettingsByCategorySchema = z.record(z.string(), z.array(DeviceSettingSchema));

export type SettingsByCategory = z.infer<typeof SettingsByCategorySchema>;
