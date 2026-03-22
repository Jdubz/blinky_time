/**
 * Pure scoring functions for music test evaluation.
 * No MCP or side-effect dependencies.
 */

import type { GroundTruth, DeviceRunScore } from './types.js';

/** Greedy nearest-neighbor matching of estimated events against reference events. */
export function matchEventsF1(
  estimated: number[],
  reference: number[],
  toleranceSec: number,
): { f1: number; precision: number; recall: number; tp: number } {
  const matched = new Set<number>();
  let tp = 0;
  for (const est of estimated) {
    let bestIdx = -1;
    let bestDist = Infinity;
    for (let i = 0; i < reference.length; i++) {
      if (matched.has(i)) continue;
      const dist = Math.abs(est - reference[i]);
      if (dist < bestDist && dist <= toleranceSec) {
        bestDist = dist;
        bestIdx = i;
      }
    }
    if (bestIdx >= 0) {
      matched.add(bestIdx);
      tp++;
    }
  }
  const precision = estimated.length > 0 ? tp / estimated.length : 0;
  const recall = reference.length > 0 ? tp / reference.length : 0;
  const f1 = (precision + recall) > 0 ? 2 * precision * recall / (precision + recall) : 0;
  return { f1, precision, recall, tp };
}

/** Computes BPM accuracy and error percentage. */
export function computeBpmMetrics(avgBpm: number, expectedBpm: number): { accuracy: number; error: number } | null {
  if (expectedBpm <= 0 || avgBpm <= 0) return null;
  const error = Math.abs(avgBpm - expectedBpm) / expectedBpm * 100;
  const accuracy = Math.max(0, 1 - error / 100);
  return { accuracy, error };
}

/** Compute mean, std, min, max of a numeric array.
 *  Returns zeros for empty arrays — callers filter upstream so this rarely triggers. */
export function computeStats(values: number[]): { mean: number; std: number; min: number; max: number } {
  if (values.length === 0) return { mean: 0, std: 0, min: 0, max: 0 };
  const mean = values.reduce((s, v) => s + v, 0) / values.length;
  const variance = values.reduce((s, v) => s + (v - mean) ** 2, 0) / values.length;
  return {
    mean,
    std: Math.sqrt(variance),
    min: Math.min(...values),
    max: Math.max(...values),
  };
}

/** Round stats to 3 decimal places for display. */
export function roundStats(s: { mean: number; std: number; min: number; max: number }) {
  return {
    mean: Math.round(s.mean * 1000) / 1000,
    std: Math.round(s.std * 1000) / 1000,
    min: Math.round(s.min * 1000) / 1000,
    max: Math.round(s.max * 1000) / 1000,
  };
}

/**
 * Robust audio latency estimation using filtered detections + histogram peak.
 * Replaces the naive all-detections median approach which was noisy.
 *
 * @param detections - Transient detections with timestampMs relative to audio start
 * @param gtHits - Ground truth hits with time in seconds and strength
 * @param audioDurationMs - Total audio duration for filtering
 * @returns Estimated latency in milliseconds, or null if insufficient data
 */
export function estimateAudioLatency(
  detections: Array<{ timestampMs: number; strength: number }>,
  gtHits: Array<{ time: number; strength: number; expectTrigger?: boolean }>,
  audioDurationMs: number,
): number | null {
  // 1. Filter to strong detections only (strength > 0.5)
  const strongDetections = detections.filter(d => d.strength > 0.5);

  // 2. Match against strong ground truth beats only (strength >= 0.8)
  const allExpected = gtHits.filter(h =>
    h.expectTrigger !== false && h.time * 1000 <= audioDurationMs
  );
  const strongExpected = allExpected.filter(h => h.strength >= 0.8);

  // Fall back to all events if not enough strong ones
  const useDetections = strongDetections.length >= 5 ? strongDetections : detections;
  const useExpected = strongExpected.length >= 3 ? strongExpected : allExpected;

  // 3. Compute offsets with tighter 350ms window
  const offsets: number[] = [];
  for (const det of useDetections) {
    let bestSignedOffset = Infinity;
    for (const hit of useExpected) {
      const hitMs = hit.time * 1000;
      const offset = det.timestampMs - hitMs;
      if (Math.abs(offset) < Math.abs(bestSignedOffset)) bestSignedOffset = offset;
    }
    if (Math.abs(bestSignedOffset) < 350) offsets.push(bestSignedOffset);
  }

  if (offsets.length < 3) return null;

  // 4. Histogram-peak estimation (10ms buckets, find mode)
  const BUCKET = 10;
  const histogram = new Map<number, number>();
  for (const o of offsets) {
    const bucket = Math.round(o / BUCKET) * BUCKET;
    histogram.set(bucket, (histogram.get(bucket) || 0) + 1);
  }

  let peakBucket = 0;
  let peakCount = 0;
  for (const [bucket, count] of histogram) {
    if (count > peakCount || (count === peakCount && Math.abs(bucket) < Math.abs(peakBucket))) {
      peakCount = count;
      peakBucket = bucket;
    }
  }

  // Refine: weighted average of offsets within ±1 bucket of peak
  let sumWeight = 0;
  let sumOffset = 0;
  for (const o of offsets) {
    if (Math.abs(Math.round(o / BUCKET) * BUCKET - peakBucket) <= BUCKET) {
      sumOffset += o;
      sumWeight++;
    }
  }

  return sumWeight > 0 ? sumOffset / sumWeight : peakBucket;
}

/**
 * Score a single device's test recording against ground truth.
 * This is the shared scoring logic used by run_music_test, run_music_test_multi,
 * and run_validation_suite.
 */
export function scoreDeviceRun(
  testData: {
    duration: number;
    startTime: number;
    transients: Array<{ timestampMs: number; type: string; strength: number }>;
    musicStates: Array<{ timestampMs: number; active: boolean; bpm: number; phase: number; confidence: number; oss?: number; plpPulse?: number }>;
    beatEvents: Array<{ timestampMs: number; bpm: number; type: string; predicted?: boolean }>;
  },
  audioStartTime: number,
  gtData: GroundTruth,
): DeviceRunScore {
  const rawDuration = testData.duration;
  const timingOffsetMs = audioStartTime - testData.startTime;

  // Adjust timestamps relative to audio start
  const detections = testData.transients
    .map(d => ({ ...d, timestampMs: d.timestampMs - timingOffsetMs }))
    .filter(d => d.timestampMs >= 0);
  const musicStates = testData.musicStates
    .map(s => ({ ...s, timestampMs: s.timestampMs - timingOffsetMs }))
    .filter(s => s.timestampMs >= 0);
  const beatEvents = testData.beatEvents
    .map(b => ({ ...b, timestampMs: b.timestampMs - timingOffsetMs }))
    .filter(b => b.timestampMs >= 0);

  // Compute audio latency using robust estimator
  const audioDurationMs = rawDuration - timingOffsetMs;
  const audioLatencyMs = estimateAudioLatency(detections, gtData.hits, audioDurationMs);
  // Use 0 offset for beat adjustment when latency estimation fails (insufficient data)
  const latencyCorrectionMs = audioLatencyMs ?? 0;

  // Beat tracking evaluation
  const audioDurationSec = audioDurationMs / 1000;
  const BEAT_TOLERANCE_SEC = 0.07;

  // Reference beats from ground truth
  const refBeats = gtData.hits
    .filter(h => h.expectTrigger !== false)
    .filter(h => h.time <= audioDurationSec)
    .map(h => h.time);

  // Estimated beats from device
  const estBeats = beatEvents.map(b => (b.timestampMs - latencyCorrectionMs) / 1000);

  // F-measure
  const { f1: beatF1, precision: beatPrecision, recall: beatRecall, tp: beatTp } =
    matchEventsF1(estBeats, refBeats, BEAT_TOLERANCE_SEC);

  // Transient F1
  const estTransients = detections.map(d => (d.timestampMs - latencyCorrectionMs) / 1000);
  const { f1: transientF1, precision: transientPrecision, recall: transientRecall } =
    matchEventsF1(estTransients, refBeats, BEAT_TOLERANCE_SEC);

  // CMLt: Continuity metric
  const correct: boolean[] = refBeats.map(ref =>
    estBeats.some(est => Math.abs(est - ref) <= BEAT_TOLERANCE_SEC)
  );

  let totalCorrectInSegments = 0;
  let longestSegment = 0;
  let currentSegment = 0;
  for (const c of correct) {
    if (c) {
      currentSegment++;
    } else {
      if (currentSegment > 0) {
        totalCorrectInSegments += currentSegment;
        longestSegment = Math.max(longestSegment, currentSegment);
        currentSegment = 0;
      }
    }
  }
  if (currentSegment > 0) {
    totalCorrectInSegments += currentSegment;
    longestSegment = Math.max(longestSegment, currentSegment);
  }
  const cmlt = refBeats.length > 0 ? totalCorrectInSegments / refBeats.length : 0;
  const cmlc = refBeats.length > 0 ? longestSegment / refBeats.length : 0;

  // AMLt: Also check half-time and double-time
  const doubleTimeBeats: number[] = [];
  for (let i = 0; i < estBeats.length; i++) {
    doubleTimeBeats.push(estBeats[i]);
    if (i < estBeats.length - 1) {
      doubleTimeBeats.push((estBeats[i] + estBeats[i + 1]) / 2);
    }
  }
  const halfTimeBeats = estBeats.filter((_, i) => i % 2 === 0);

  let bestAmlCorrect = correct;
  for (const altEst of [doubleTimeBeats, halfTimeBeats]) {
    const altCorrect = refBeats.map(ref =>
      altEst.some(est => Math.abs(est - ref) <= BEAT_TOLERANCE_SEC)
    );
    if (altCorrect.filter(Boolean).length > bestAmlCorrect.filter(Boolean).length) {
      bestAmlCorrect = altCorrect;
    }
  }

  let amlTotal = 0;
  let amlLongest = 0;
  let amlCurrent = 0;
  for (const c of bestAmlCorrect) {
    if (c) {
      amlCurrent++;
    } else {
      if (amlCurrent > 0) {
        amlTotal += amlCurrent;
        amlLongest = Math.max(amlLongest, amlCurrent);
        amlCurrent = 0;
      }
    }
  }
  if (amlCurrent > 0) {
    amlTotal += amlCurrent;
    amlLongest = Math.max(amlLongest, amlCurrent);
  }
  const amlt = refBeats.length > 0 ? amlTotal / refBeats.length : 0;

  // Music mode metrics
  const activeStates = musicStates.filter(s => s.active);
  const avgBpm = activeStates.length > 0
    ? activeStates.reduce((sum, s) => sum + s.bpm, 0) / activeStates.length : 0;
  const avgConf = activeStates.length > 0
    ? activeStates.reduce((sum, s) => sum + s.confidence, 0) / activeStates.length : 0;

  const expectedBPM = gtData.bpm || 0;
  const bpmMetrics = computeBpmMetrics(avgBpm, expectedBPM);

  // Phase stability
  let phaseStability = 0;
  if (activeStates.length > 1) {
    const phaseDiffs: number[] = [];
    for (let i = 1; i < activeStates.length; i++) {
      let diff = activeStates[i].phase - activeStates[i - 1].phase;
      if (diff < -0.5) diff += 1.0;
      if (diff > 0.5) diff -= 1.0;
      phaseDiffs.push(diff);
    }
    if (phaseDiffs.length > 0) {
      const meanDiff = phaseDiffs.reduce((s, d) => s + d, 0) / phaseDiffs.length;
      const variance = phaseDiffs.reduce((s, d) => s + (d - meanDiff) ** 2, 0) / phaseDiffs.length;
      phaseStability = Math.max(0, 1 - Math.sqrt(variance) * 10);
    }
  }

  // PLP accuracy metrics
  const plpValues = activeStates.filter(s => s.plpPulse !== undefined).map(s => s.plpPulse!);
  let plpAtTransient = 0;
  let plpAutoCorr = 0;
  let plpPeakiness = 0;
  let plpMean = 0;

  if (plpValues.length > 0) {
    plpMean = plpValues.reduce((s, v) => s + v, 0) / plpValues.length;
    const plpMax = Math.max(...plpValues);
    plpPeakiness = plpMean > 0.01 ? plpMax / plpMean : 0;

    // PLP value at transient times: for each transient, find nearest music state
    // Use a sliding search start index since both arrays are sorted by time
    const transientPlpValues: number[] = [];
    let searchStart = 0;
    for (const det of detections) {
      let bestState: (typeof activeStates)[0] | null = null;
      let bestDist = Infinity;
      for (let si = searchStart; si < activeStates.length; si++) {
        const dist = Math.abs(activeStates[si].timestampMs - det.timestampMs);
        if (dist < bestDist) { bestDist = dist; bestState = activeStates[si]; }
        else if (dist > bestDist) { searchStart = Math.max(0, si - 2); break; }  // past minimum, advance start
      }
      if (bestState && bestState.plpPulse !== undefined && bestDist < 100) {
        transientPlpValues.push(bestState.plpPulse);
      }
    }
    if (transientPlpValues.length > 0) {
      plpAtTransient = transientPlpValues.reduce((s, v) => s + v, 0) / transientPlpValues.length;
    }

    // PLP autocorrelation at detected BPM lag
    if (avgBpm > 0 && plpValues.length > 10) {
      const streamRate = plpValues.length / (audioDurationSec || 1);
      const bpmLag = Math.round(streamRate * 60 / avgBpm);
      if (bpmLag > 0 && bpmLag < plpValues.length / 2) {
        let sumXY = 0, sumX2 = 0;
        const n = plpValues.length - bpmLag;
        for (let i = 0; i < n; i++) {
          const x = plpValues[i] - plpMean;
          const y = plpValues[i + bpmLag] - plpMean;
          sumXY += x * y;
          sumX2 += x * x;
        }
        plpAutoCorr = sumX2 > 0 ? sumXY / sumX2 : 0;
      }
    }
  }

  // Diagnostics
  const transientBeatOffsets: number[] = [];
  detections.forEach((det) => {
    const detSec = (det.timestampMs - latencyCorrectionMs) / 1000;
    let bestOffset = Infinity;
    for (const ref of refBeats) {
      const offset = detSec - ref;
      if (Math.abs(offset) < Math.abs(bestOffset)) bestOffset = offset;
    }
    if (Math.abs(bestOffset) < 0.5) {
      transientBeatOffsets.push(Math.round(bestOffset * 1000));
    }
  });

  let phaseOffsetStats: { median: number; stdDev: number; iqr: number } | null = null;
  if (transientBeatOffsets.length >= 3) {
    const sorted = [...transientBeatOffsets].sort((a, b) => a - b);
    const median = sorted[Math.floor(sorted.length / 2)];
    const mean = sorted.reduce((s, v) => s + v, 0) / sorted.length;
    const stdDev = Math.sqrt(sorted.reduce((s, v) => s + (v - mean) ** 2, 0) / sorted.length);
    const q1 = sorted[Math.floor(sorted.length * 0.25)];
    const q3 = sorted[Math.floor(sorted.length * 0.75)];
    phaseOffsetStats = { median: Math.round(median), stdDev: Math.round(stdDev), iqr: Math.round(q3 - q1) };
  }

  const beatEventOffsets: number[] = [];
  estBeats.forEach((est) => {
    let bestOffset = Infinity;
    for (const ref of refBeats) {
      const offset = est - ref;
      if (Math.abs(offset) < Math.abs(bestOffset)) bestOffset = offset;
    }
    if (Math.abs(bestOffset) < 0.5) {
      beatEventOffsets.push(Math.round(bestOffset * 1000));
    }
  });

  let beatOffsetStats: { median: number; stdDev: number; iqr: number } | null = null;
  if (beatEventOffsets.length >= 3) {
    const sorted = [...beatEventOffsets].sort((a, b) => a - b);
    const median = sorted[Math.floor(sorted.length / 2)];
    const mean = sorted.reduce((s, v) => s + v, 0) / sorted.length;
    const stdDev = Math.sqrt(sorted.reduce((s, v) => s + (v - mean) ** 2, 0) / sorted.length);
    const q1 = sorted[Math.floor(sorted.length * 0.25)];
    const q3 = sorted[Math.floor(sorted.length * 0.75)];
    beatOffsetStats = { median: Math.round(median), stdDev: Math.round(stdDev), iqr: Math.round(q3 - q1) };
  }

  const beatOffsetHistogram: Record<string, number> = {};
  for (const offset of beatEventOffsets) {
    const bucket = Math.round(offset / 10) * 10;
    const key = `${bucket}`;
    beatOffsetHistogram[key] = (beatOffsetHistogram[key] || 0) + 1;
  }

  const predictedBeats = beatEvents.filter(b => b.predicted === true).length;
  const fallbackBeats = beatEvents.filter(b => b.predicted === false || b.predicted === undefined).length;

  return {
    audioLatencyMs,
    audioDurationSec,
    timingOffsetMs,
    beatTracking: {
      f1: Math.round(beatF1 * 1000) / 1000,
      precision: Math.round(beatPrecision * 1000) / 1000,
      recall: Math.round(beatRecall * 1000) / 1000,
      cmlt: Math.round(cmlt * 1000) / 1000,
      cmlc: Math.round(cmlc * 1000) / 1000,
      amlt: Math.round(amlt * 1000) / 1000,
      refBeats: refBeats.length,
      estBeats: estBeats.length,
    },
    transientTracking: {
      f1: Math.round(transientF1 * 1000) / 1000,
      precision: Math.round(transientPrecision * 1000) / 1000,
      recall: Math.round(transientRecall * 1000) / 1000,
      count: detections.length,
    },
    musicMode: {
      avgBpm: Math.round(avgBpm * 10) / 10,
      expectedBpm: expectedBPM,
      bpmError: bpmMetrics ? Math.round(bpmMetrics.error * 10) / 10 : null,
      bpmAccuracy: bpmMetrics ? Math.round(bpmMetrics.accuracy * 1000) / 1000 : null,
      avgConfidence: Math.round(avgConf * 100) / 100,
      phaseStability: Math.round(phaseStability * 1000) / 1000,
      activationMs: activeStates.length > 0 ? activeStates[0].timestampMs : null,
    },
    plp: {
      atTransient: Math.round(plpAtTransient * 1000) / 1000,  // avg PLP pulse when transients fire (1.0 = perfect alignment)
      autoCorr: Math.round(plpAutoCorr * 1000) / 1000,        // autocorrelation at BPM lag (1.0 = perfectly periodic)
      peakiness: Math.round(plpPeakiness * 100) / 100,        // peak/mean ratio (1.0 = flat/cosine, >2 = strong pattern)
      mean: Math.round(plpMean * 1000) / 1000,                // average PLP value (0.5 = cosine fallback)
    },
    diagnostics: {
      transientRate: audioDurationSec > 0 ? detections.length / audioDurationSec : 0,
      expectedBeatRate: audioDurationSec > 0 ? refBeats.length / audioDurationSec : 0,
      beatEventRate: audioDurationSec > 0 ? estBeats.length / audioDurationSec : 0,
      phaseOffsetStats,
      beatOffsetStats,
      beatOffsetHistogram,
      beatVsReference: {
        matched: beatTp,
        extra: estBeats.length - beatTp,
        missed: refBeats.length - beatTp,
      },
      predictionRatio: beatEvents.length > 0 ? { predicted: predictedBeats, fallback: fallbackBeats, total: beatEvents.length } : null,
      transientBeatOffsets,
      beatEventOffsets,
    },
    adjustedDetections: detections,
    adjustedBeatEvents: beatEvents,
    adjustedMusicStates: musicStates,
  };
}

/**
 * Convert a DeviceRunScore into a compact summary object.
 * Shared by run_music_test (single & multi-run) and run_music_test_multi.
 */
export function formatScoreSummary(score: DeviceRunScore) {
  return {
    beatTracking: {
      f1: score.beatTracking.f1,
      precision: score.beatTracking.precision,
      recall: score.beatTracking.recall,
      refBeats: score.beatTracking.refBeats,
      estBeats: score.beatTracking.estBeats,
    },
    transientTracking: score.transientTracking,
    musicMode: score.musicMode,
    plp: score.plp,
    diagnostics: {
      transientRate: Math.round(score.diagnostics.transientRate * 10) / 10,
      expectedBeatRate: Math.round(score.diagnostics.expectedBeatRate * 10) / 10,
      beatEventRate: Math.round(score.diagnostics.beatEventRate * 10) / 10,
      transientOffsetMs: score.diagnostics.phaseOffsetStats,
      beatOffsetMs: score.diagnostics.beatOffsetStats,
      beatOffsetHistogram: score.diagnostics.beatOffsetHistogram,
      predictionRatio: score.diagnostics.predictionRatio,
      matched: score.diagnostics.beatVsReference.matched,
      extra: score.diagnostics.beatVsReference.extra,
      missed: score.diagnostics.beatVsReference.missed,
    },
    timing: { latencyMs: score.audioLatencyMs !== null ? Math.round(score.audioLatencyMs) : null },
  };
}
