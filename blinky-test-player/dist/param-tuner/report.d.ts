/**
 * Report Generation
 * Generates summary reports from tuning results
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * Reports on ensemble detection performance.
 * Legacy per-mode reporting has been removed.
 */
import type { TunerOptions } from './types.js';
import { StateManager } from './state.js';
export declare function generateReport(optionsOrOutputDir: TunerOptions | string, stateManager: StateManager): Promise<void>;
/**
 * Show a summary of the generated report
 */
export declare function showReportSummary(outputDir: string): void;
