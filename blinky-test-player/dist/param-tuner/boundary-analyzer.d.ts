/**
 * Boundary Analyzer
 *
 * Detects when optimized parameter values hit or approach their defined limits.
 * Generates warnings and recommendations for range expansion.
 */
import type { SweepResult } from './types.js';
import { StateManager } from './state.js';
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
/**
 * Analyze a single parameter's sweep result for boundary issues
 */
export declare function analyzeParameterBoundary(paramName: string, sweepResult: SweepResult): BoundaryWarning | null;
/**
 * Analyze all sweep results for boundary issues
 */
export declare function analyzeBoundaries(stateManager: StateManager): BoundaryReport;
/**
 * Print boundary analysis report to console
 */
export declare function printBoundaryReport(report: BoundaryReport): void;
/**
 * Generate firmware update recommendations based on boundary analysis
 */
export declare function generateBoundaryRecommendations(report: BoundaryReport): string[];
