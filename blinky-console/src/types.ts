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
  ConfiguredDevice,
  // Settings
  SettingType,
  DeviceSetting,
  SettingsResponse,
  SettingsByCategory,
  // Audio
  AudioSample,
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
  // Common
  ConnectionState,
  GeneratorType,
  EffectType,
  GeneratorState,
  EffectState,
} from './schemas';

// Re-export schemas for runtime validation and type guards
export {
  DeviceInfoSchema,
  isDeviceConfigured,
  SettingTypeSchema,
  DeviceSettingSchema,
  SettingsResponseSchema,
  SettingsByCategorySchema,
  AudioSampleSchema,
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
  ConnectionStateSchema,
  GeneratorTypeSchema,
  EffectTypeSchema,
  GeneratorStateSchema,
  EffectStateSchema,
} from './schemas';
