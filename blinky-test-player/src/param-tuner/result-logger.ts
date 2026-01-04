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

import { promises as fs } from 'fs';
import { join } from 'path';
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

export class ResultLogger {
  private logPath: string;

  constructor(outputDir: string) {
    this.logPath = join(outputDir, 'tuning-results.json');
  }

  /**
   * Load existing log or create new one
   */
  async load(): Promise<ResultLog> {
    try {
      const content = await fs.readFile(this.logPath, 'utf-8');
      return JSON.parse(content);
    } catch (err) {
      // File doesn't exist - create new log
      const now = new Date().toISOString();
      return {
        version: '1.0',
        created: now,
        lastUpdated: now,
        entries: [],
      };
    }
  }

  /**
   * Save log to disk
   */
  async save(log: ResultLog): Promise<void> {
    log.lastUpdated = new Date().toISOString();
    const content = JSON.stringify(log, null, 2);
    await fs.writeFile(this.logPath, content, 'utf-8');
  }

  /**
   * Append a sweep result to the log
   */
  async logSweepResult(result: SweepResult, refinementUsed: boolean = false): Promise<void> {
    const log = await this.load();

    const entry: ResultLogEntry = {
      timestamp: new Date().toISOString(),
      parameter: result.parameter,
      mode: result.mode,
      optimalValue: result.optimal.value,
      optimalF1: result.optimal.avgF1,
      refinementUsed,
      totalPointsTested: result.sweep.length,
      fullSweep: result,
    };

    log.entries.push(entry);
    await this.save(log);
  }

  /**
   * Get the most recent result for a parameter
   */
  async getLatestResult(parameterName: string): Promise<ResultLogEntry | null> {
    const log = await this.load();
    const entries = log.entries.filter(e => e.parameter === parameterName);
    if (entries.length === 0) return null;

    // Return most recent
    return entries[entries.length - 1];
  }

  /**
   * Get all historical results for a parameter
   */
  async getHistory(parameterName: string): Promise<ResultLogEntry[]> {
    const log = await this.load();
    return log.entries.filter(e => e.parameter === parameterName);
  }

  /**
   * Generate a markdown summary of recent results (for commit messages, PRs, etc.)
   */
  async generateSummary(limit: number = 10): Promise<string> {
    const log = await this.load();
    const recent = log.entries.slice(-limit);

    let summary = `# Recent Parameter Tuning Results\n\n`;
    summary += `Last updated: ${log.lastUpdated}\n`;
    summary += `Total entries: ${log.entries.length}\n\n`;
    summary += `## Recent ${Math.min(limit, recent.length)} Results\n\n`;
    summary += `| Parameter | Mode | Optimal Value | F1 Score | Refined | Points Tested | Date |\n`;
    summary += `|-----------|------|---------------|----------|---------|---------------|------|\n`;

    for (const entry of recent.reverse()) {
      const date = new Date(entry.timestamp).toLocaleDateString();
      const refined = entry.refinementUsed ? 'Yes' : 'No';
      summary += `| ${entry.parameter} | ${entry.mode} | ${entry.optimalValue} | ${entry.optimalF1.toFixed(3)} | ${refined} | ${entry.totalPointsTested} | ${date} |\n`;
    }

    return summary;
  }

  /**
   * Compare current results to previous best
   */
  async compareToHistory(parameterName: string, currentF1: number): Promise<{
    improved: boolean;
    delta: number;
    previousF1: number | null;
  }> {
    const history = await this.getHistory(parameterName);
    if (history.length === 0) {
      return { improved: true, delta: 0, previousF1: null };
    }

    // Find best previous F1
    const previousBest = Math.max(...history.map(e => e.optimalF1));
    const delta = currentF1 - previousBest;

    return {
      improved: delta > 0,
      delta,
      previousF1: previousBest,
    };
  }
}
