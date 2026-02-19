/**
 * Beat Tracking Evaluation Metrics
 *
 * Implements standard beat tracking evaluation metrics ported from mir_eval.beat:
 * - F-measure with configurable tolerance (default 70ms, standard for beat tracking)
 * - CMLt (Correct Metrical Level, total): Fraction of beats in correctly-tracked continuous segments
 * - AMLt (Allowed Metrical Levels, total): Same but allows half/double time as correct
 * - BPM accuracy: |detected - true| / true
 *
 * References:
 * - Davies et al. (2009) "Evaluation Methods for Musical Audio Beat Tracking Algorithms"
 * - mir_eval: https://craffel.github.io/mir_eval/
 */

/** A single beat event with timestamp */
export interface BeatEvent {
  /** Time in seconds */
  time: number;
}

/** Complete beat evaluation results */
export interface BeatEvalResult {
  /** F-measure at 70ms tolerance (standard beat tracking metric) */
  fMeasure: number;
  /** Precision at 70ms tolerance */
  precision: number;
  /** Recall at 70ms tolerance */
  recall: number;
  /** CMLt: Fraction of beats in correct continuous segments */
  cmlt: number;
  /** CMLc: Longest correct continuous segment ratio */
  cmlc: number;
  /** AMLt: Like CMLt but allows half/double/triple time */
  amlt: number;
  /** AMLc: Like CMLc but allows half/double/triple time */
  amlc: number;
  /** BPM accuracy: 1.0 - |detected - true| / true (1.0 = perfect) */
  bpmAccuracy: number | null;
  /** Detected BPM from inter-beat intervals */
  detectedBpm: number | null;
  /** Expected BPM (from ground truth) */
  expectedBpm: number | null;
  /** Number of reference beats */
  refBeats: number;
  /** Number of estimated beats */
  estBeats: number;
  /** Timing tolerance used (seconds) */
  toleranceSec: number;
}

/**
 * Default tolerance window for beat tracking evaluation.
 * 70ms is the standard used in MIREX beat tracking evaluations.
 * This is much tighter than the 200-350ms used for onset detection.
 */
const DEFAULT_TOLERANCE_SEC = 0.07;

/**
 * Compute F-measure for beat tracking.
 *
 * Matches estimated beats to reference beats within a tolerance window.
 * Each reference beat can only be matched once (greedy nearest-neighbor).
 */
function computeFMeasure(
  refBeats: number[],
  estBeats: number[],
  toleranceSec: number,
): { fMeasure: number; precision: number; recall: number } {
  if (refBeats.length === 0 && estBeats.length === 0) {
    return { fMeasure: 1.0, precision: 1.0, recall: 1.0 };
  }
  if (refBeats.length === 0 || estBeats.length === 0) {
    return { fMeasure: 0.0, precision: 0.0, recall: 0.0 };
  }

  // Match estimated beats to reference beats
  const matchedRef = new Set<number>();
  let truePositives = 0;

  // Sort estimated beats and try to match each to nearest unmatched reference
  const sortedEst = [...estBeats].sort((a, b) => a - b);
  const sortedRef = [...refBeats].sort((a, b) => a - b);

  for (const est of sortedEst) {
    let bestIdx = -1;
    let bestDist = Infinity;

    for (let i = 0; i < sortedRef.length; i++) {
      if (matchedRef.has(i)) continue;
      const dist = Math.abs(est - sortedRef[i]);
      if (dist < bestDist && dist <= toleranceSec) {
        bestDist = dist;
        bestIdx = i;
      }
    }

    if (bestIdx >= 0) {
      matchedRef.add(bestIdx);
      truePositives++;
    }
  }

  const precision = estBeats.length > 0 ? truePositives / estBeats.length : 0;
  const recall = refBeats.length > 0 ? truePositives / refBeats.length : 0;
  const fMeasure =
    precision + recall > 0
      ? (2 * precision * recall) / (precision + recall)
      : 0;

  return { fMeasure, precision, recall };
}

/**
 * Compute continuity-based metrics (CMLt, CMLc, AMLt, AMLc).
 *
 * CML (Correct Metrical Level):
 * - Measures how long the tracker stays correctly locked
 * - CMLt = fraction of reference beats in correct continuous segments
 * - CMLc = longest correct continuous segment / total reference beats
 *
 * AML (Allowed Metrical Levels):
 * - Same as CML but also accepts half-time, double-time, and triple-time
 * - Catches "octave errors" where tracker locks to correct beat grid
 *   but at wrong metrical level
 */
function computeContinuity(
  refBeats: number[],
  estBeats: number[],
  toleranceSec: number,
): { cmlt: number; cmlc: number; amlt: number; amlc: number } {
  if (refBeats.length < 2 || estBeats.length < 2) {
    return { cmlt: 0, cmlc: 0, amlt: 0, amlc: 0 };
  }

  const sortedRef = [...refBeats].sort((a, b) => a - b);
  const sortedEst = [...estBeats].sort((a, b) => a - b);

  // Compute reference inter-beat intervals
  const refIBIs: number[] = [];
  for (let i = 1; i < sortedRef.length; i++) {
    refIBIs.push(sortedRef[i] - sortedRef[i - 1]);
  }

  // For CML: check if each reference beat has a matching estimated beat
  const cmlCorrect = computeCorrectBeats(sortedRef, sortedEst, toleranceSec);
  const { total: cmlt, longest: cmlc } = computeContinuityScores(
    cmlCorrect,
    sortedRef.length,
  );

  // For AML: also try half-time, double-time, and triple-time offsets
  // Generate alternative estimated beat sequences
  const altEstSequences = generateMetricalAlternatives(sortedEst);

  let bestAmlCorrect = cmlCorrect; // Start with original
  for (const altEst of altEstSequences) {
    const altCorrect = computeCorrectBeats(sortedRef, altEst, toleranceSec);
    // Use whichever has more correct beats
    const altTotal = altCorrect.filter(Boolean).length;
    const bestTotal = bestAmlCorrect.filter(Boolean).length;
    if (altTotal > bestTotal) {
      bestAmlCorrect = altCorrect;
    }
  }

  const { total: amlt, longest: amlc } = computeContinuityScores(
    bestAmlCorrect,
    sortedRef.length,
  );

  return { cmlt, cmlc, amlt, amlc };
}

/**
 * For each reference beat, check if there's a matching estimated beat
 * within tolerance. Returns boolean array.
 */
function computeCorrectBeats(
  refBeats: number[],
  estBeats: number[],
  toleranceSec: number,
): boolean[] {
  const correct: boolean[] = new Array(refBeats.length).fill(false);

  // Use a pointer approach for efficiency (both arrays sorted)
  let estIdx = 0;
  for (let refIdx = 0; refIdx < refBeats.length; refIdx++) {
    // Advance est pointer to nearest beat
    while (
      estIdx < estBeats.length - 1 &&
      Math.abs(estBeats[estIdx + 1] - refBeats[refIdx]) <
        Math.abs(estBeats[estIdx] - refBeats[refIdx])
    ) {
      estIdx++;
    }

    if (
      estIdx < estBeats.length &&
      Math.abs(estBeats[estIdx] - refBeats[refIdx]) <= toleranceSec
    ) {
      correct[refIdx] = true;
    }
  }

  return correct;
}

/**
 * From a boolean array of correct/incorrect beats, compute:
 * - total: fraction of beats in any continuous correct segment (>= 1 beat)
 * - longest: fraction of beats in the longest continuous correct segment
 */
function computeContinuityScores(
  correct: boolean[],
  totalBeats: number,
): { total: number; longest: number } {
  if (totalBeats === 0) return { total: 0, longest: 0 };

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
  // Handle trailing segment
  if (currentSegment > 0) {
    totalCorrectInSegments += currentSegment;
    longestSegment = Math.max(longestSegment, currentSegment);
  }

  return {
    total: totalCorrectInSegments / totalBeats,
    longest: longestSegment / totalBeats,
  };
}

/**
 * Generate metrical alternatives for AML evaluation.
 * Creates half-time, double-time, and triple-time versions of beat sequence.
 */
function generateMetricalAlternatives(estBeats: number[]): number[][] {
  if (estBeats.length < 2) return [];

  const alternatives: number[][] = [];

  // Double-time: insert beats between each pair
  const doubleTime: number[] = [];
  for (let i = 0; i < estBeats.length; i++) {
    doubleTime.push(estBeats[i]);
    if (i < estBeats.length - 1) {
      doubleTime.push((estBeats[i] + estBeats[i + 1]) / 2);
    }
  }
  alternatives.push(doubleTime);

  // Half-time: take every other beat (try both even and odd offsets)
  const halfTimeEven: number[] = [];
  const halfTimeOdd: number[] = [];
  for (let i = 0; i < estBeats.length; i++) {
    if (i % 2 === 0) halfTimeEven.push(estBeats[i]);
    else halfTimeOdd.push(estBeats[i]);
  }
  alternatives.push(halfTimeEven);
  alternatives.push(halfTimeOdd);

  // Triple-time: take every third beat
  for (let offset = 0; offset < 3; offset++) {
    const tripleTime: number[] = [];
    for (let i = offset; i < estBeats.length; i += 3) {
      tripleTime.push(estBeats[i]);
    }
    if (tripleTime.length >= 2) {
      alternatives.push(tripleTime);
    }
  }

  return alternatives;
}

/**
 * Estimate BPM from beat timestamps using median inter-beat interval.
 */
function estimateBpm(beats: number[]): number | null {
  if (beats.length < 2) return null;

  const sorted = [...beats].sort((a, b) => a - b);
  const ibis: number[] = [];
  for (let i = 1; i < sorted.length; i++) {
    ibis.push(sorted[i] - sorted[i - 1]);
  }

  // Use median IBI for robustness
  ibis.sort((a, b) => a - b);
  const medianIBI = ibis[Math.floor(ibis.length / 2)];

  return medianIBI > 0 ? 60.0 / medianIBI : null;
}

/**
 * Evaluate beat tracking performance.
 *
 * @param refBeats Reference (ground truth) beat timestamps in seconds
 * @param estBeats Estimated (detected) beat timestamps in seconds
 * @param expectedBpm Known BPM from ground truth (optional)
 * @param toleranceSec Tolerance window in seconds (default: 0.07 = 70ms)
 */
export function evaluateBeats(
  refBeats: number[],
  estBeats: number[],
  expectedBpm?: number,
  toleranceSec: number = DEFAULT_TOLERANCE_SEC,
): BeatEvalResult {
  // Compute F-measure
  const refTimes = refBeats.filter((t) => t >= 0);
  const estTimes = estBeats.filter((t) => t >= 0);

  const { fMeasure, precision, recall } = computeFMeasure(
    refTimes,
    estTimes,
    toleranceSec,
  );

  // Compute continuity metrics
  const { cmlt, cmlc, amlt, amlc } = computeContinuity(
    refTimes,
    estTimes,
    toleranceSec,
  );

  // BPM accuracy
  const detectedBpm = estimateBpm(estTimes);
  const bpmRef = expectedBpm || estimateBpm(refTimes);
  let bpmAccuracy: number | null = null;
  if (detectedBpm !== null && bpmRef !== null && bpmRef > 0) {
    const bpmError = Math.abs(detectedBpm - bpmRef) / bpmRef;
    bpmAccuracy = Math.max(0, 1.0 - bpmError);
  }

  return {
    fMeasure: round3(fMeasure),
    precision: round3(precision),
    recall: round3(recall),
    cmlt: round3(cmlt),
    cmlc: round3(cmlc),
    amlt: round3(amlt),
    amlc: round3(amlc),
    bpmAccuracy: bpmAccuracy !== null ? round3(bpmAccuracy) : null,
    detectedBpm: detectedBpm !== null ? round1(detectedBpm) : null,
    expectedBpm: bpmRef !== null ? round1(bpmRef) : null,
    refBeats: refTimes.length,
    estBeats: estTimes.length,
    toleranceSec,
  };
}

/**
 * Evaluate beat tracking from device music mode state.
 *
 * Converts device beat events (from phase wrapping) to timestamps
 * and evaluates against ground truth beat times.
 *
 * @param refBeats Ground truth beat timestamps in seconds
 * @param deviceBeatTimesMs Device-detected beat event timestamps in milliseconds
 * @param expectedBpm Known BPM (optional)
 * @param audioLatencyMs Estimated audio latency to compensate (optional)
 */
export function evaluateDeviceBeats(
  refBeats: number[],
  deviceBeatTimesMs: number[],
  expectedBpm?: number,
  audioLatencyMs: number = 0,
): BeatEvalResult {
  // Convert device beat times from ms to seconds, applying latency correction
  const estBeats = deviceBeatTimesMs.map(
    (ms) => (ms - audioLatencyMs) / 1000,
  );

  return evaluateBeats(refBeats, estBeats, expectedBpm);
}

function round3(v: number): number {
  return Math.round(v * 1000) / 1000;
}

function round1(v: number): number {
  return Math.round(v * 10) / 10;
}
