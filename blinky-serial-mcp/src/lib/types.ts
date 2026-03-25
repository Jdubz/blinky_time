/**
 * Shared types for music test scoring.
 */

/** Ground truth type shared by music test scoring. */
export type GroundTruth = {
  pattern: string;
  durationMs: number;
  bpm?: number;
  hits: Array<{ time: number; type: string; strength: number; expectTrigger?: boolean }>;
  /** Onset consensus times (from 5-system onset detection). Used for transientF1. */
  onsets?: Array<{ time: number; strength: number }>;
};

/** Result of scoring a single device run against ground truth. */
export interface DeviceRunScore {
  audioLatencyMs: number | null;
  audioDurationSec: number;
  timingOffsetMs: number;
  beatTracking: {
    f1: number; precision: number; recall: number;
    cmlt: number; cmlc: number; amlt: number;
    refBeats: number; estBeats: number;
  };
  transientTracking: {
    f1: number; precision: number; recall: number; count: number;
  };
  musicMode: {
    avgBpm: number; expectedBpm: number;
    bpmError: number | null; bpmAccuracy: number | null;
    avgConfidence: number; phaseStability: number;
    activationMs: number | null;
  };
  diagnostics: {
    transientRate: number; expectedBeatRate: number; beatEventRate: number;
    phaseOffsetStats: { median: number; stdDev: number; iqr: number } | null;
    beatOffsetStats: { median: number; stdDev: number; iqr: number } | null;
    beatOffsetHistogram: Record<string, number>;
    beatVsReference: { matched: number; extra: number; missed: number };
    predictionRatio: { predicted: number; fallback: number; total: number } | null;
    transientBeatOffsets: number[];
    beatEventOffsets: number[];
  };
  plp: {
    atTransient: number;  // avg PLP pulse when transients fire (1.0 = perfect alignment)
    autoCorr: number;     // autocorrelation at BPM lag (1.0 = perfectly periodic)
    peakiness: number;    // peak/mean ratio (1.0 = flat, >2 = strong pattern)
    mean: number;         // average PLP value (0.5 = cosine fallback)
  };
  // Adjusted raw data
  adjustedDetections: Array<{ timestampMs: number; type: string; strength: number }>;
  adjustedBeatEvents: Array<{ timestampMs: number; bpm: number; type: string; predicted?: boolean }>;
  adjustedMusicStates: Array<{ timestampMs: number; active: boolean; bpm: number; phase: number; confidence: number; plpPulse?: number }>;
}
