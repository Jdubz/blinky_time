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
  l: number; // level (0-1, post-AGC output)
  t: number; // percussion strength (0-1+, max of kick/snare/hihat, single-frame impulse)
  r: number; // RMS tracked level (0-1, what AGC is targeting)
  s: number; // software gain (AGC multiplier, typically 0.01-100)
  h: number; // hardware gain (PDM gain setting, 0-80)
  k: 0 | 1; // kick impulse (boolean flag)
  sn: 0 | 1; // snare impulse (boolean flag)
  hh: 0 | 1; // hihat impulse (boolean flag)
  ks: number; // kick strength (0.0-1.0)
  ss: number; // snare strength (0.0-1.0)
  hs: number; // hihat strength (0.0-1.0)
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

// Connection state
export type ConnectionState = 'disconnected' | 'connecting' | 'connected' | 'error';

// Settings grouped by category
export interface SettingsByCategory {
  [category: string]: DeviceSetting[];
}
