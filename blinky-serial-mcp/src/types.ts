/**
 * Types for blinky device serial communication
 */

export interface DeviceInfo {
  device: string;
  version: string;
  width: number;
  height: number;
  leds: number;
}

export interface AudioSample {
  l: number;      // Level (0-1)
  t: number;      // Transient strength (0-1)
  pk: number;     // Peak level (0-1)
  vl: number;     // Valley level (0-1)
  raw: number;    // Raw ADC level (0-1)
  h: number;      // Hardware gain (0-80)
  alive: number;  // PDM status (0 or 1)
  z: number;      // Zero-crossing rate (0-1)
  // Debug fields (only present in debug stream mode)
  avg?: number;   // Recent average level (for threshold calculation)
  prev?: number;  // Previous frame level (for attack detection)
  agree?: number; // Detector agreement count (0-7, how many detectors fired)
  conf?: number;  // Ensemble confidence (0-1, combined confidence score)
}

export interface MusicModeState {
  a: number;      // Active (0 or 1)
  bpm: number;    // Tempo (BPM)
  ph: number;     // Phase (0-1)
  str: number;    // Rhythm strength (0-1)
  conf: number;   // CBSS beat tracking confidence (0-1)
  bc: number;     // Beat count (tracked beats)
  q: number;      // Beat event (0 or 1, phase wrap)
  e: number;      // Energy (0-1)
  p: number;      // Pulse (0-1)
  // Observability fields (always present in stream)
  cb?: number;    // Current CBSS value
  oss?: number;   // Smoothed onset strength
  ttb?: number;   // Time to next beat (frames countdown)
  bp?: number;    // Beat was predicted (0=fallback, 1=predicted)
  // Debug fields (only present in debug stream mode)
  ps?: number;    // Periodicity strength (raw autocorrelation peak)
  sb?: number;    // Stable beats count
  mb?: number;    // Missed beats count
  pe?: number;    // Peak energy
  ei?: number;    // Error integral
}

export interface LedTelemetry {
  tot: number;    // Total heat (sum of all heat values)
  pct: number;    // Brightness percent (0-100)
}

export interface BeatEvent {
  timestampMs: number;
  bpm: number;
  type: 'quarter';
  predicted?: boolean;  // Whether beat came from CBSS prediction vs fallback
}

export interface MusicModeMetrics {
  bpmAccuracy: number;       // % accuracy vs expected BPM
  expectedBPM: number;
  detectedBPM: number;
  activationTimeMs: number;  // Time until active=true
  beatF1Score: number;       // Beat detection F1
  confidenceAvg: number;     // Average confidence while active
}

export interface BatteryStatus {
  n: boolean;     // Connected
  c: boolean;     // Charging
  v: number;      // Voltage
  p: number;      // Percent
}

export interface Setting {
  name: string;
  value: number;
  type: 'uint8' | 'uint16' | 'int8' | 'float';
  cat: string;
  min: number;
  max: number;
  desc?: string;
}

export interface ConnectionState {
  connected: boolean;
  port: string | null;
  deviceInfo: DeviceInfo | null;
  streaming: boolean;
}

export interface TestResult {
  pattern: string;
  duration: number;
  detections: TransientEvent[];
  metrics: TestMetrics;
}

export interface TransientEvent {
  timestampMs: number;
  type: 'low' | 'high' | 'unified';
  strength: number;
}

export interface TestMetrics {
  f1Score: number;
  precision: number;
  recall: number;
  truePositives: number;
  falsePositives: number;
  falseNegatives: number;
  expectedTotal: number;
  avgTimingErrorMs: number | null;
  audioLatencyMs: number; // Estimated systematic audio latency (median offset)
}

export interface TimestampedSample {
  timestampMs: number;
  level: number;
  raw: number;
  transient: number;  // Unified transient strength from "Drummer's Algorithm"
}

export interface TimestampedMusicState {
  timestampMs: number;
  active: boolean;
  bpm: number;
  phase: number;
  confidence: number;
  oss?: number;     // Smoothed onset strength
  cbss?: number;    // Current CBSS value
}
