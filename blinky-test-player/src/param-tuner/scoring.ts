/**
 * Detection scoring - ground truth matching and F1/precision/recall calculation
 *
 * Extracted from runner.ts to enable reuse across single-device and multi-device runners.
 */

import type { TestResult } from './types.js';
import type { TransientEvent } from './device-connection.js';

export interface GroundTruthData {
  pattern: string;
  durationMs: number;
  startedAt: string;
  hits: Array<{ timeMs: number; type: string; strength: number; expectTrigger?: boolean }>;
}

const TIMING_TOLERANCE_MS = 350;
const STRONG_BEAT_THRESHOLD = 0.8;

/**
 * Score detections against ground truth.
 *
 * @param detections - Transient events detected by the device
 * @param recordStartTime - Timestamp (ms since epoch) when recording started
 * @param groundTruth - Ground truth data from the test player
 * @param patternId - Pattern ID for the result
 * @param rawDurationMs - Raw recording duration as fallback
 * @returns TestResult with F1, precision, recall, and timing metrics
 */
export function scoreDetections(
  detections: TransientEvent[],
  recordStartTime: number,
  groundTruth: GroundTruthData,
  patternId: string,
  rawDurationMs: number,
): TestResult {
  // Calculate timing offset
  let timingOffsetMs = 0;
  let correctedDetections = detections;

  if (groundTruth.startedAt && recordStartTime) {
    const audioStartTime = new Date(groundTruth.startedAt).getTime();
    timingOffsetMs = audioStartTime - recordStartTime;

    correctedDetections = detections.map(d => ({
      ...d,
      timestampMs: d.timestampMs - timingOffsetMs,
    })).filter(d => d.timestampMs >= 0);
  }

  const allHits = groundTruth.hits || [];

  const expectedHits = allHits.filter((h) => {
    if (typeof h.expectTrigger === 'boolean') {
      return h.expectTrigger;
    }
    return h.strength >= STRONG_BEAT_THRESHOLD;
  });

  // Estimate audio latency
  const offsets: number[] = [];
  correctedDetections.forEach((detection) => {
    let minDist = Infinity;
    let closestOffset = 0;
    expectedHits.forEach((expected) => {
      if (detection.type !== 'unified' && expected.type !== detection.type) return;
      const offset = detection.timestampMs - expected.timeMs;
      if (Math.abs(offset) < Math.abs(minDist)) {
        minDist = offset;
        closestOffset = offset;
      }
    });
    if (Math.abs(minDist) < 1000) {
      offsets.push(closestOffset);
    }
  });

  let audioLatencyMs = 0;
  if (offsets.length > 0) {
    offsets.sort((a, b) => a - b);
    audioLatencyMs = offsets[Math.floor(offsets.length / 2)];
  }

  // Match detections to expected hits
  const matchedExpected = new Set<number>();
  const matchedDetections = new Set<number>();
  const matchPairs = new Map<number, { expectedIdx: number; timingError: number }>();

  correctedDetections.forEach((detection, dIdx) => {
    let bestMatchIdx = -1;
    let bestMatchDist = Infinity;
    const correctedTime = detection.timestampMs - audioLatencyMs;

    expectedHits.forEach((expected, eIdx) => {
      if (matchedExpected.has(eIdx)) return;
      if (detection.type !== 'unified' && expected.type !== detection.type) return;

      const dist = Math.abs(correctedTime - expected.timeMs);
      if (dist < bestMatchDist && dist <= TIMING_TOLERANCE_MS) {
        bestMatchDist = dist;
        bestMatchIdx = eIdx;
      }
    });

    if (bestMatchIdx >= 0) {
      matchedExpected.add(bestMatchIdx);
      matchedDetections.add(dIdx);
      matchPairs.set(dIdx, { expectedIdx: bestMatchIdx, timingError: bestMatchDist });
    }
  });

  const truePositives = matchedDetections.size;
  const falsePositives = correctedDetections.length - truePositives;
  const falseNegatives = expectedHits.length - truePositives;

  const precision = correctedDetections.length > 0 ? truePositives / correctedDetections.length : 0;
  const recall = expectedHits.length > 0 ? truePositives / expectedHits.length : 0;
  const f1 = (precision + recall) > 0
    ? 2 * (precision * recall) / (precision + recall)
    : 0;

  let totalTimingError = 0;
  matchPairs.forEach(({ timingError }) => {
    totalTimingError += timingError;
  });
  const avgTimingErrorMs = matchPairs.size > 0 ? totalTimingError / matchPairs.size : null;

  return {
    pattern: patternId,
    durationMs: groundTruth.durationMs || rawDurationMs,
    f1: Math.round(f1 * 1000) / 1000,
    precision: Math.round(precision * 1000) / 1000,
    recall: Math.round(recall * 1000) / 1000,
    truePositives,
    falsePositives,
    falseNegatives,
    expectedTotal: expectedHits.length,
    avgTimingErrorMs: avgTimingErrorMs !== null ? Math.round(avgTimingErrorMs) : null,
    audioLatencyMs: Math.round(audioLatencyMs),
  };
}
