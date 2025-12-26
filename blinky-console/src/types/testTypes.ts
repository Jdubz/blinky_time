/**
 * Types for percussion detection testing system
 */

export type PercussionType = 'kick' | 'snare' | 'hihat';

/**
 * Ground truth annotation from CSV file
 */
export interface GroundTruthHit {
  time: number; // Time in seconds
  type: PercussionType;
  strength: number; // 0.0 - 1.0
}

/**
 * Detection event from device via serial
 */
export interface DetectionEvent {
  timestampMs: number; // Timestamp from device (millis())
  type: PercussionType;
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
 * Per-type metrics breakdown
 */
export interface TypeMetrics {
  kick: TestMetrics;
  snare: TestMetrics;
  hihat: TestMetrics;
  overall: TestMetrics;
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
