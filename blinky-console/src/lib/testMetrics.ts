/**
 * Transient detection test metrics calculation
 */

import type {
  GroundTruthHit,
  DetectionEvent,
  TestMetrics,
  TypeMetrics,
  TransientType,
} from '../types/testTypes';

const TOLERANCE_MS = 50; // ±50ms window for matching

/**
 * Calculate metrics for a specific transient type (low or high band)
 */
export function calculateTypeMetrics(
  groundTruth: GroundTruthHit[],
  detections: DetectionEvent[],
  type: TransientType
): TestMetrics {
  // Filter by type
  const gtHits = groundTruth.filter(gt => gt.type === type);
  const detHits = detections.filter(det => det.type === type);

  let truePositives = 0;
  let falsePositives = 0;
  let falseNegatives = 0;
  const timingErrors: number[] = [];

  const matchedGT = new Set<number>();
  const matchedDet = new Set<number>();

  // Match each detection to nearest ground truth within tolerance
  detHits.forEach((det, detIdx) => {
    const detTimeS = det.timestampMs / 1000;

    let bestMatch: { gtIdx: number; error: number } | null = null;
    let minError = TOLERANCE_MS / 1000;

    gtHits.forEach((gt, gtIdx) => {
      if (matchedGT.has(gtIdx)) return;

      const error = Math.abs(gt.time - detTimeS);
      if (error < minError) {
        minError = error;
        bestMatch = { gtIdx, error };
      }
    });

    if (bestMatch) {
      // True positive
      truePositives++;
      matchedGT.add((bestMatch as { gtIdx: number; error: number }).gtIdx);
      matchedDet.add(detIdx);
      timingErrors.push((bestMatch as { gtIdx: number; error: number }).error * 1000); // Convert to ms
    } else {
      // False positive
      falsePositives++;
    }
  });

  // Unmatched ground truth = false negatives
  falseNegatives = gtHits.length - matchedGT.size;

  // Calculate metrics
  const precision = truePositives / (truePositives + falsePositives) || 0;
  const recall = truePositives / (truePositives + falseNegatives) || 0;
  const f1Score = (2 * (precision * recall)) / (precision + recall) || 0;

  const avgTimingErrorMs =
    timingErrors.length > 0
      ? timingErrors.reduce((a, b) => a + b, 0) / timingErrors.length
      : undefined;

  return {
    precision,
    recall,
    f1Score,
    truePositives,
    falsePositives,
    falseNegatives,
    avgTimingErrorMs,
  };
}

/**
 * Calculate metrics for all transient types (low and high bands)
 */
export function calculateAllMetrics(
  groundTruth: GroundTruthHit[],
  detections: DetectionEvent[]
): TypeMetrics {
  const low = calculateTypeMetrics(groundTruth, detections, 'low');
  const high = calculateTypeMetrics(groundTruth, detections, 'high');

  // Calculate overall metrics from combined counts
  const totalTP = low.truePositives + high.truePositives;
  const totalFP = low.falsePositives + high.falsePositives;
  const totalFN = low.falseNegatives + high.falseNegatives;

  const precision = totalTP / (totalTP + totalFP) || 0;
  const recall = totalTP / (totalTP + totalFN) || 0;
  const f1Score = (2 * precision * recall) / (precision + recall) || 0;

  // Calculate timing error as weighted average
  const timingErrors = [low.avgTimingErrorMs, high.avgTimingErrorMs].filter(
    (x): x is number => x !== undefined
  );

  const avgTimingErrorMs =
    timingErrors.length > 0
      ? timingErrors.reduce((a, b) => a + b) / timingErrors.length
      : undefined;

  const overall: TestMetrics = {
    precision,
    recall,
    f1Score,
    truePositives: totalTP,
    falsePositives: totalFP,
    falseNegatives: totalFN,
    avgTimingErrorMs,
  };

  return { low, high, overall };
}

/**
 * Export test results as CSV
 */
export function exportResultsCSV(testName: string, metrics: TypeMetrics): string {
  const lines = [
    `Test: ${testName}`,
    `Date: ${new Date().toISOString()}`,
    '',
    'Overall Metrics:',
    `F1 Score,${(metrics.overall.f1Score * 100).toFixed(1)}%`,
    `Precision,${(metrics.overall.precision * 100).toFixed(1)}%`,
    `Recall,${(metrics.overall.recall * 100).toFixed(1)}%`,
    `True Positives,${metrics.overall.truePositives}`,
    `False Positives,${metrics.overall.falsePositives}`,
    `False Negatives,${metrics.overall.falseNegatives}`,
    `Avg Timing Error,${metrics.overall.avgTimingErrorMs?.toFixed(2) ?? 'N/A'}ms`,
    '',
    'Per-Band Metrics:',
    'Band,F1,Precision,Recall,TP,FP,FN,Avg Timing Error (ms)',
    `Low (50-200Hz),${(metrics.low.f1Score * 100).toFixed(1)}%,${(metrics.low.precision * 100).toFixed(1)}%,${(metrics.low.recall * 100).toFixed(1)}%,${metrics.low.truePositives},${metrics.low.falsePositives},${metrics.low.falseNegatives},${metrics.low.avgTimingErrorMs?.toFixed(2) ?? 'N/A'}`,
    `High (2-8kHz),${(metrics.high.f1Score * 100).toFixed(1)}%,${(metrics.high.precision * 100).toFixed(1)}%,${(metrics.high.recall * 100).toFixed(1)}%,${metrics.high.truePositives},${metrics.high.falsePositives},${metrics.high.falseNegatives},${metrics.high.avgTimingErrorMs?.toFixed(2) ?? 'N/A'}`,
  ];

  return lines.join('\n');
}
