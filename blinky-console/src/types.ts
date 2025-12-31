/**
 * Type definitions for blinky-console
 *
 * All types are derived from Zod schemas for runtime validation.
 * See src/schemas/ for schema definitions.
 */

// Re-export all types from schemas
export type {
  // Device
  DeviceInfo,
  // Settings
  SettingType,
  DeviceSetting,
  SettingsResponse,
  SettingsByCategory,
  // Audio
  AudioSample,
  RhythmData,
  MusicModeData,
  AudioMessage,
  // Battery
  BatterySample,
  BatteryMessage,
  BatteryStatusData,
  BatteryStatusResponse,
  // Events
  DetectionMode,
  TransientMessage,
  RhythmMessage,
  StatusMessage,
  PercussionMessage,
  // Common
  ConnectionState,
  GeneratorType,
  EffectType,
  GeneratorState,
  EffectState,
} from './schemas';

// Re-export schemas for runtime validation
export {
  DeviceInfoSchema,
  SettingTypeSchema,
  DeviceSettingSchema,
  SettingsResponseSchema,
  SettingsByCategorySchema,
  AudioSampleSchema,
  RhythmDataSchema,
  MusicModeDataSchema,
  AudioMessageSchema,
  BatterySampleSchema,
  BatteryMessageSchema,
  BatteryStatusDataSchema,
  BatteryStatusResponseSchema,
  DetectionModeSchema,
  TransientMessageSchema,
  RhythmMessageSchema,
  StatusMessageSchema,
  PercussionMessageSchema,
  ConnectionStateSchema,
  GeneratorTypeSchema,
  EffectTypeSchema,
  GeneratorStateSchema,
  EffectStateSchema,
} from './schemas';
