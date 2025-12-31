/**
 * Types for transient detection testing system
 * Simplified single-band amplitude spike detection
 */

export type TransientType = 'transient';

/**
 * Ground truth annotation for test patterns
 */
export interface GroundTruthHit {
  time: number; // Time in seconds
  type: TransientType;
  strength: number; // 0.0 - 1.0
}

/**
 * Detection event from device via serial
 */
export interface DetectionEvent {
  timestampMs: number; // Timestamp from device (millis())
  type: TransientType;
  strength: number;
  matched?: boolean; // Set during metrics calculation
}

/**
 * Test metrics calculated from ground truth vs detections
 */
export interface TestMetrics {
  precision: number; // TP / (TP + FP)
  recall: number; // TP / (TP + FN)
  f1Score: number; // 2 * P * R / (P + R)
  truePositives: number;
  falsePositives: number;
  falseNegatives: number;
  avgTimingErrorMs?: number;
}

/**
 * Metrics breakdown for simplified single-band detection
 */
export interface TypeMetrics {
  transient: TestMetrics; // Simplified amplitude spike detection
  overall: TestMetrics; // Overall metrics (same as transient for single-band)
}

/**
 * Programmatic test pattern with built-in ground truth
 */
export interface TestPattern {
  id: string;
  name: string;
  description: string;
  durationMs: number; // Total pattern duration
  bpm?: number; // Optional BPM for musical patterns
  hits: GroundTruthHit[]; // Automatically serves as ground truth
}

/**
 * Test run state
 */
export interface TestRun {
  id: string;
  testName: string;
  startTime: number;
  endTime?: number;
  groundTruth: GroundTruthHit[];
  detections: DetectionEvent[];
  metrics?: TypeMetrics;
}
