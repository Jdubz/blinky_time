/**
 * Percussion detection test metrics calculation
 */

import type {
  GroundTruthHit,
  DetectionEvent,
  TestMetrics,
  TypeMetrics,
  PercussionType,
} from '../types/testTypes';

const TOLERANCE_MS = 50; // ±50ms window for matching

/**
 * Calculate metrics for a specific percussion type
 */
export function calculateTypeMetrics(
  groundTruth: GroundTruthHit[],
  detections: DetectionEvent[],
  type: PercussionType
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
 * Calculate metrics for all percussion types
 */
export function calculateAllMetrics(
  groundTruth: GroundTruthHit[],
  detections: DetectionEvent[]
): TypeMetrics {
  const kick = calculateTypeMetrics(groundTruth, detections, 'kick');
  const snare = calculateTypeMetrics(groundTruth, detections, 'snare');
  const hihat = calculateTypeMetrics(groundTruth, detections, 'hihat');

  // Fix BUG #2: Calculate overall metrics from combined counts, not averages
  const totalTP = kick.truePositives + snare.truePositives + hihat.truePositives;
  const totalFP = kick.falsePositives + snare.falsePositives + hihat.falsePositives;
  const totalFN = kick.falseNegatives + snare.falseNegatives + hihat.falseNegatives;

  const precision = totalTP / (totalTP + totalFP) || 0;
  const recall = totalTP / (totalTP + totalFN) || 0;
  const f1Score = (2 * precision * recall) / (precision + recall) || 0;

  // Fix BUG #3: Calculate timing error correctly
  const timingErrors = [
    kick.avgTimingErrorMs,
    snare.avgTimingErrorMs,
    hihat.avgTimingErrorMs,
  ].filter((x): x is number => x !== undefined);

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

  return { kick, snare, hihat, overall };
}

/**
 * Parse CSV ground truth file
 */
export async function parseGroundTruthCSV(file: File): Promise<GroundTruthHit[]> {
  const text = await file.text();
  const lines = text.trim().split('\n');

  // Fix BUG #7: Better header detection
  const hasHeader = /^time\s*,\s*type\s*,\s*strength/i.test(lines[0]);
  const dataLines = hasHeader ? lines.slice(1) : lines;

  const validTypes = ['kick', 'snare', 'hihat'];
  let skippedLines = 0;

  const hits = dataLines
    .filter(line => line.trim().length > 0)
    .map((line, idx) => {
      const [time, type, strength] = line.split(',').map(s => s.trim());

      const timeValue = parseFloat(time);
      const strengthValue = parseFloat(strength);
      const percType = type.toLowerCase();

      // Fix BUG #8: Warn about malformed lines
      if (isNaN(timeValue) || isNaN(strengthValue)) {
        console.warn(`Skipping malformed CSV line ${idx + 2}: ${line}`);
        skippedLines++;
        return null;
      }

      // Fix BUG #6: Validate percussion type
      if (!validTypes.includes(percType)) {
        console.warn(
          `Skipping invalid percussion type "${type}" at time ${timeValue}s (line ${idx + 2})`
        );
        skippedLines++;
        return null;
      }

      return {
        time: timeValue,
        type: percType as PercussionType,
        strength: strengthValue,
      };
    })
    .filter((hit): hit is GroundTruthHit => hit !== null);

  // Fix BUG #8: Notify user of skipped lines
  if (skippedLines > 0) {
    console.warn(`Loaded ${hits.length} annotations (${skippedLines} lines skipped due to errors)`);
  }

  return hits;
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
    'Per-Type Metrics:',
    'Type,F1,Precision,Recall,TP,FP,FN,Avg Timing Error (ms)',
    `Kick,${(metrics.kick.f1Score * 100).toFixed(1)}%,${(metrics.kick.precision * 100).toFixed(1)}%,${(metrics.kick.recall * 100).toFixed(1)}%,${metrics.kick.truePositives},${metrics.kick.falsePositives},${metrics.kick.falseNegatives},${metrics.kick.avgTimingErrorMs?.toFixed(2) ?? 'N/A'}`,
    `Snare,${(metrics.snare.f1Score * 100).toFixed(1)}%,${(metrics.snare.precision * 100).toFixed(1)}%,${(metrics.snare.recall * 100).toFixed(1)}%,${metrics.snare.truePositives},${metrics.snare.falsePositives},${metrics.snare.falseNegatives},${metrics.snare.avgTimingErrorMs?.toFixed(2) ?? 'N/A'}`,
    `Hihat,${(metrics.hihat.f1Score * 100).toFixed(1)}%,${(metrics.hihat.precision * 100).toFixed(1)}%,${(metrics.hihat.recall * 100).toFixed(1)}%,${metrics.hihat.truePositives},${metrics.hihat.falsePositives},${metrics.hihat.falseNegatives},${metrics.hihat.avgTimingErrorMs?.toFixed(2) ?? 'N/A'}`,
  ];

  return lines.join('\n');
}
