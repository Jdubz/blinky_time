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
  lo: number;     // Low band onset (0 or 1)
  hi: number;     // High band onset (0 or 1)
  los: number;    // Low band strength (0-1)
  his: number;    // High band strength (0-1)
  z: number;      // Zero-crossing rate (0-1)
  // Debug fields (only present in debug stream mode)
  lob?: number;   // Low band baseline
  hib?: number;   // High band baseline
  lop?: number;   // Low band previous energy
  hip?: number;   // High band previous energy
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
  type: 'low' | 'high';
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
}
