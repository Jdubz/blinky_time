/**
 * Zod schemas for firmware ↔ console JSON contract
 *
 * This module provides runtime validation for all JSON messages
 * exchanged between the blinky-things firmware and blinky-console.
 *
 * Usage:
 *   import { DeviceInfoSchema, SettingsResponseSchema } from './schemas';
 *
 *   const result = DeviceInfoSchema.safeParse(jsonData);
 *   if (result.success) {
 *     const deviceInfo = result.data; // Typed as DeviceInfo
 *   } else {
 *     console.error('Validation failed:', result.error);
 *   }
 */

// Device information
export { DeviceInfoSchema, type DeviceInfo } from './device';

// Settings
export {
  SettingTypeSchema,
  DeviceSettingSchema,
  SettingsResponseSchema,
  SettingsByCategorySchema,
  type SettingType,
  type DeviceSetting,
  type SettingsResponse,
  type SettingsByCategory,
} from './settings';

// Audio streaming
export {
  AudioSampleSchema,
  RhythmDataSchema,
  MusicModeDataSchema,
  AudioMessageSchema,
  type AudioSample,
  type RhythmData,
  type MusicModeData,
  type AudioMessage,
} from './audio';

// Battery streaming
export {
  BatterySampleSchema,
  BatteryMessageSchema,
  BatteryStatusDataSchema,
  BatteryStatusResponseSchema,
  type BatterySample,
  type BatteryMessage,
  type BatteryStatusData,
  type BatteryStatusResponse,
} from './battery';

// Event messages
export {
  DetectionModeSchema,
  TransientMessageSchema,
  RhythmMessageSchema,
  StatusMessageSchema,
  type DetectionMode,
  type TransientMessage,
  type RhythmMessage,
  type StatusMessage,
} from './events';

// Common types
export {
  ConnectionStateSchema,
  GeneratorTypeSchema,
  EffectTypeSchema,
  GeneratorStateSchema,
  EffectStateSchema,
  type ConnectionState,
  type GeneratorType,
  type EffectType,
  type GeneratorState,
  type EffectState,
} from './common';
