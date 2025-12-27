// Device information from `json info` command
export interface DeviceInfo {
  device: string;
  version: string;
  width: number;
  height: number;
  leds: number;
}

// Setting types supported by firmware
export type SettingType = 'uint8' | 'int8' | 'uint16' | 'uint32' | 'float' | 'bool';

// Individual setting from `json settings` command
export interface DeviceSetting {
  name: string;
  value: number | boolean;
  type: SettingType;
  cat: string;
  min: number;
  max: number;
  desc?: string; // Optional description from firmware
}

// Response from `json settings` command
export interface SettingsResponse {
  settings: DeviceSetting[];
}

// Audio sample from streaming `{"a":{...}}` messages
export interface AudioSample {
  l: number; // level (0-1, post-range-mapping output)
  t: number; // transient strength (0-1, max of low/high band onsets)
  pk: number; // peak level (current tracked peak for window normalization, raw 0-1 range)
  vl: number; // valley level (current tracked valley for window normalization, raw 0-1 range)
  raw: number; // raw ADC level (what HW gain targets, 0-1 range)
  h: number; // hardware gain (PDM gain setting, 0-80)
  alive: 0 | 1; // PDM status (0=dead, 1=alive)
  z: number; // zero-crossing rate (0.0-1.0)
}

// Streaming audio message format
export interface AudioMessage {
  a: AudioSample;
}

// Battery status from streaming `{"b":{...}}` messages
export interface BatterySample {
  n: boolean; // connected (battery detected)
  c: boolean; // charging
  v: number; // voltage
  p: number; // percent (0-100)
}

// Streaming battery message format
export interface BatteryMessage {
  b: BatterySample;
}

// Transient detection message from `{"type":"TRANSIENT",...}` messages
// Simplified single-band amplitude spike detection
export interface TransientMessage {
  type: 'TRANSIENT';
  timestampMs: number;
  strength: number; // Transient strength (0-1, LOUD + SUDDEN spike detection)
}

// Legacy percussion message type for backwards compatibility
export type PercussionMessage = TransientMessage;

// Connection state
export type ConnectionState = 'disconnected' | 'connecting' | 'connected' | 'error';

// Settings grouped by category
export interface SettingsByCategory {
  [category: string]: DeviceSetting[];
}
