/**
 * Result Logger: Maintains a committed JSON log of all parameter tuning results
 *
 * This replaces the manual PARAMETER_TUNING_HISTORY.md file with an automated
 * JSON log that tracks all sweep results over time.
 *
 * Features:
 * - Append-only log format (preserves historical data)
 * - Git-friendly (can be committed for version control)
 * - Structured data (easy to query and analyze)
 * - Timestamped entries
 */
import type { SweepResult } from './types.js';
export interface ResultLogEntry {
    timestamp: string;
    parameter: string;
    mode: string;
    optimalValue: number;
    optimalF1: number;
    refinementUsed: boolean;
    totalPointsTested: number;
    fullSweep: SweepResult;
}
export interface ResultLog {
    version: string;
    created: string;
    lastUpdated: string;
    entries: ResultLogEntry[];
}
export declare class ResultLogger {
    private logPath;
    constructor(outputDir: string);
    /**
     * Load existing log or create new one
     */
    load(): Promise<ResultLog>;
    /**
     * Save log to disk
     */
    save(log: ResultLog): Promise<void>;
    /**
     * Append a sweep result to the log
     */
    logSweepResult(result: SweepResult, refinementUsed?: boolean): Promise<void>;
    /**
     * Get the most recent result for a parameter
     */
    getLatestResult(parameterName: string): Promise<ResultLogEntry | null>;
    /**
     * Get all historical results for a parameter
     */
    getHistory(parameterName: string): Promise<ResultLogEntry[]>;
    /**
     * Generate a markdown summary of recent results (for commit messages, PRs, etc.)
     */
    generateSummary(limit?: number): Promise<string>;
    /**
     * Compare current results to previous best
     */
    compareToHistory(parameterName: string, currentF1: number): Promise<{
        improved: boolean;
        delta: number;
        previousF1: number | null;
    }>;
}
