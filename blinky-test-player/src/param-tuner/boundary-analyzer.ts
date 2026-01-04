/**
 * Boundary Analyzer
 *
 * Detects when optimized parameter values hit or approach their defined limits.
 * Generates warnings and recommendations for range expansion.
 */

import type { SweepResult, ParameterDef } from './types.js';
import { PARAMETERS } from './types.js';
import { StateManager } from './state.js';

// =============================================================================
// TYPES
// =============================================================================

/**
 * Severity levels for boundary warnings
 */
export type BoundarySeverity = 'critical' | 'warning' | 'info';

/**
 * A single boundary warning
 */
export interface BoundaryWarning {
  /** Parameter name */
  parameter: string;
  /** Severity level */
  severity: BoundarySeverity;
  /** Type of boundary issue */
  type: 'at-min' | 'at-max' | 'near-min' | 'near-max';
  /** Current optimal value */
  optimalValue: number;
  /** Distance to boundary (0 = at boundary) */
  distanceToBoundary: number;
  /** Distance as percentage of range */
  distancePercent: number;
  /** Boundary value (min or max) */
  boundaryValue: number;
  /** Recommended new boundary */
  recommendedBoundary: number;
  /** Human-readable message */
  message: string;
}

/**
 * Complete boundary analysis report
 */
export interface BoundaryReport {
  /** Timestamp of analysis */
  timestamp: string;
  /** Number of parameters analyzed */
  totalAnalyzed: number;
  /** Warnings by severity */
  warnings: {
    critical: BoundaryWarning[];
    warning: BoundaryWarning[];
    info: BoundaryWarning[];
  };
  /** Has any critical or warning issues */
  hasIssues: boolean;
  /** Summary message */
  summary: string;
}

// =============================================================================
// CONFIGURATION
// =============================================================================

/**
 * Threshold for "near boundary" warnings (percentage of range)
 */
const NEAR_BOUNDARY_THRESHOLD = 0.10; // 10%

/**
 * Extension factor for recommended new boundaries
 */
const BOUNDARY_EXTENSION_FACTOR = 0.5; // Extend by 50% of current range

// =============================================================================
// ANALYSIS FUNCTIONS
// =============================================================================

/**
 * Analyze a single parameter's sweep result for boundary issues
 */
export function analyzeParameterBoundary(
  paramName: string,
  sweepResult: SweepResult
): BoundaryWarning | null {
  const paramDef = PARAMETERS[paramName];
  if (!paramDef) {
    return null;
  }

  const optimalValue = sweepResult.optimal.value;
  const { min, max } = paramDef;
  const range = max - min;

  // Check if at exact boundary
  if (optimalValue === min) {
    return {
      parameter: paramName,
      severity: 'critical',
      type: 'at-min',
      optimalValue,
      distanceToBoundary: 0,
      distancePercent: 0,
      boundaryValue: min,
      recommendedBoundary: min - (range * BOUNDARY_EXTENSION_FACTOR),
      message: `${paramName}: optimal ${optimalValue} is AT minimum boundary (${min}). Recommend extending min to ${(min - (range * BOUNDARY_EXTENSION_FACTOR)).toFixed(2)}`,
    };
  }

  if (optimalValue === max) {
    return {
      parameter: paramName,
      severity: 'critical',
      type: 'at-max',
      optimalValue,
      distanceToBoundary: 0,
      distancePercent: 0,
      boundaryValue: max,
      recommendedBoundary: max + (range * BOUNDARY_EXTENSION_FACTOR),
      message: `${paramName}: optimal ${optimalValue} is AT maximum boundary (${max}). Recommend extending max to ${(max + (range * BOUNDARY_EXTENSION_FACTOR)).toFixed(2)}`,
    };
  }

  // Check if near boundary
  const distanceToMin = optimalValue - min;
  const distanceToMax = max - optimalValue;
  const nearThreshold = range * NEAR_BOUNDARY_THRESHOLD;

  if (distanceToMin < nearThreshold) {
    const distancePercent = (distanceToMin / range) * 100;
    return {
      parameter: paramName,
      severity: 'warning',
      type: 'near-min',
      optimalValue,
      distanceToBoundary: distanceToMin,
      distancePercent,
      boundaryValue: min,
      recommendedBoundary: min - (range * BOUNDARY_EXTENSION_FACTOR),
      message: `${paramName}: optimal ${optimalValue} is ${distancePercent.toFixed(1)}% from min (${min}). Consider extending min.`,
    };
  }

  if (distanceToMax < nearThreshold) {
    const distancePercent = (distanceToMax / range) * 100;
    return {
      parameter: paramName,
      severity: 'warning',
      type: 'near-max',
      optimalValue,
      distanceToBoundary: distanceToMax,
      distancePercent,
      boundaryValue: max,
      recommendedBoundary: max + (range * BOUNDARY_EXTENSION_FACTOR),
      message: `${paramName}: optimal ${optimalValue} is ${distancePercent.toFixed(1)}% from max (${max}). Consider extending max.`,
    };
  }

  return null;
}

/**
 * Analyze all sweep results for boundary issues
 */
export function analyzeBoundaries(stateManager: StateManager): BoundaryReport {
  const warnings: BoundaryReport['warnings'] = {
    critical: [],
    warning: [],
    info: [],
  };

  let totalAnalyzed = 0;

  // Check each parameter with sweep results
  for (const paramName of Object.keys(PARAMETERS)) {
    const sweepResult = stateManager.getSweepResult(paramName);
    if (!sweepResult) continue;

    totalAnalyzed++;
    const warning = analyzeParameterBoundary(paramName, sweepResult);
    if (warning) {
      warnings[warning.severity].push(warning);
    }
  }

  const hasIssues = warnings.critical.length > 0 || warnings.warning.length > 0;

  let summary: string;
  if (warnings.critical.length > 0) {
    summary = `CRITICAL: ${warnings.critical.length} parameter(s) at boundary limits. Range expansion strongly recommended.`;
  } else if (warnings.warning.length > 0) {
    summary = `WARNING: ${warnings.warning.length} parameter(s) near boundary limits. Consider range expansion.`;
  } else {
    summary = `All ${totalAnalyzed} parameters within acceptable ranges.`;
  }

  return {
    timestamp: new Date().toISOString(),
    totalAnalyzed,
    warnings,
    hasIssues,
    summary,
  };
}

/**
 * Print boundary analysis report to console
 */
export function printBoundaryReport(report: BoundaryReport): void {
  console.log('\n' + '='.repeat(60));
  console.log('  BOUNDARY ANALYSIS');
  console.log('='.repeat(60));

  if (report.warnings.critical.length > 0) {
    console.log('\n  CRITICAL - Parameters AT boundary limits:');
    for (const w of report.warnings.critical) {
      console.log(`    - ${w.parameter}: ${w.optimalValue} (at ${w.type === 'at-min' ? 'min' : 'max'} = ${w.boundaryValue})`);
      console.log(`      Recommend: extend ${w.type === 'at-min' ? 'min' : 'max'} to ${w.recommendedBoundary.toFixed(2)}`);
    }
  }

  if (report.warnings.warning.length > 0) {
    console.log('\n  WARNING - Parameters near boundary limits:');
    for (const w of report.warnings.warning) {
      console.log(`    - ${w.parameter}: ${w.optimalValue} (${w.distancePercent.toFixed(1)}% from ${w.type.includes('min') ? 'min' : 'max'})`);
    }
  }

  if (!report.hasIssues) {
    console.log('\n  All parameters within acceptable ranges.');
  }

  console.log('\n  Summary: ' + report.summary);
  console.log('='.repeat(60) + '\n');
}

/**
 * Generate firmware update recommendations based on boundary analysis
 */
export function generateBoundaryRecommendations(report: BoundaryReport): string[] {
  const recommendations: string[] = [];

  for (const warning of [...report.warnings.critical, ...report.warnings.warning]) {
    const paramDef = PARAMETERS[warning.parameter];
    if (!paramDef) continue;

    if (warning.type === 'at-min' || warning.type === 'near-min') {
      recommendations.push(
        `Update ${warning.parameter} minimum from ${paramDef.min} to ${warning.recommendedBoundary.toFixed(2)} in types.ts`
      );
    } else {
      recommendations.push(
        `Update ${warning.parameter} maximum from ${paramDef.max} to ${warning.recommendedBoundary.toFixed(2)} in types.ts`
      );
    }
  }

  return recommendations;
}
