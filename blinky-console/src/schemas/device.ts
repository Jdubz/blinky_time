/**
 * Device information Zod schemas
 *
 * Validates responses from the `json info` command.
 */

import { z } from 'zod';

/**
 * Device information from `json info` command
 */
export const DeviceInfoSchema = z.object({
  device: z.string().min(1, 'Device name is required'),
  version: z.string().min(1, 'Version is required'),
  width: z.number().int().positive(),
  height: z.number().int().positive(),
  leds: z.number().int().positive(),
});

export type DeviceInfo = z.infer<typeof DeviceInfoSchema>;
