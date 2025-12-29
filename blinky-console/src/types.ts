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

// Rhythm analyzer data from streaming `{"r":{...}}` messages
export interface RhythmData {
  bpm: number; // Detected BPM (beats per minute)
  str: number; // Periodicity strength (0-1, confidence in detected tempo)
  per: number; // Detected period in milliseconds
  lik: number; // Beat likelihood (0-1, current position in beat cycle)
  ph: number; // Phase (0-1 within detected period)
  buf: number; // Buffer fill level (0-256 frames)
}

// Music mode data from streaming `{"m":{...}}` messages
export interface MusicModeData {
  a: 0 | 1; // Active (0=inactive, 1=active/pattern detected)
  bpm: number; // Tempo in beats per minute
  ph: number; // Phase (0-1 within current beat)
  conf: number; // Confidence (0-1, pattern reliability)
  q: 0 | 1; // Quarter note event (1 = event this frame)
  h: 0 | 1; // Half note event (1 = event this frame)
  w: 0 | 1; // Whole note event (1 = event this frame)
}

// Streaming audio message format
export interface AudioMessage {
  a: AudioSample;
  r?: RhythmData; // Optional rhythm analyzer data
  m?: MusicModeData; // Optional music mode data
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
export interface TransientMessage {
  type: 'TRANSIENT';
  ts: number; // Timestamp in milliseconds
  strength: number; // Transient strength (0-1, LOUD + SUDDEN spike detection)
  mode: number; // Detection mode (0=drummer, 1=bass, 2=HFC, 3=flux, 4=hybrid)
  level: number; // Normalized level (0-1)
  energy: number; // Mode-specific energy value (bass level, flux value, etc.)
  // Legacy field for backwards compatibility
  timestampMs?: number;
}

// Rhythm analyzer telemetry from `{"type":"RHYTHM",...}` messages
export interface RhythmMessage {
  type: 'RHYTHM';
  bpm: number; // Detected BPM
  strength: number; // Periodicity strength (0-1)
  periodMs: number; // Detected period in milliseconds
  likelihood: number; // Beat likelihood (0-1)
  phase: number; // Current phase (0-1)
  bufferFill: number; // Buffer fill level (0-256)
}

// System status telemetry from `{"type":"STATUS",...}` messages
export interface StatusMessage {
  type: 'STATUS';
  ts: number; // Timestamp in milliseconds
  mode: number; // Detection mode (0-4)
  hwGain: number; // Hardware gain (0-80)
  level: number; // Current level (0-1)
  avgLevel: number; // Recent average level (0-1)
  peakLevel: number; // Peak level (0-1)
}

// Legacy percussion message type for backwards compatibility
export type PercussionMessage = TransientMessage;

// Connection state
export type ConnectionState = 'disconnected' | 'connecting' | 'connected' | 'error';

// Settings grouped by category
export interface SettingsByCategory {
  [category: string]: DeviceSetting[];
}
