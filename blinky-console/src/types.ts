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
}

// Response from `json settings` command
export interface SettingsResponse {
  settings: DeviceSetting[];
}

// Audio sample from streaming `{"a":{...}}` messages
export interface AudioSample {
  l: number; // level (0-1)
  t: number; // transient (0-1)
  e: number; // envelope (0-1)
  g: number; // gain multiplier
}

// Streaming audio message format
export interface AudioMessage {
  a: AudioSample;
}

// Connection state
export type ConnectionState = 'disconnected' | 'connecting' | 'connected' | 'error';

// Console log entry
export interface ConsoleEntry {
  id: number;
  timestamp: Date;
  type: 'sent' | 'received' | 'error' | 'info';
  message: string;
}

// Settings grouped by category
export interface SettingsByCategory {
  [category: string]: DeviceSetting[];
}
