/**
 * Phase 5: Report Generation
 * Generates comprehensive reports from tuning results
 */
import { StateManager } from './state.js';
export declare function generateReport(outputDir: string, stateManager: StateManager): Promise<void>;
export declare function showReportSummary(outputDir: string): void;
