/**
 * Test Queue Management System
 *
 * Enables scheduling multiple test suites in sequence with:
 * - Persistent queue state for resume after interruption
 * - Progress tracking across suites
 * - Automatic retry on failure
 * - Summary report after all suites complete
 */

import { existsSync, readFileSync, writeFileSync, unlinkSync } from 'fs';
import { join } from 'path';
import type { TunerOptions } from './types.js';
import type { SuiteConfig, SuiteStatus } from './suite.js';
import { SuiteRunner, PREDEFINED_SUITES, getSuite } from './suite.js';

// =============================================================================
// QUEUE STATE TYPES
// =============================================================================

/**
 * Status of a queued suite
 */
export type QueuedSuiteStatus = 'pending' | 'running' | 'completed' | 'failed' | 'skipped';

/**
 * A suite in the queue
 */
export interface QueuedSuite {
  /** Suite ID (references predefined or custom suite) */
  suiteId: string;
  /** Queue position */
  position: number;
  /** Execution status */
  status: QueuedSuiteStatus;
  /** Start time (ISO string) */
  startedAt?: string;
  /** End time (ISO string) */
  completedAt?: string;
  /** Suite result */
  result?: SuiteStatus;
  /** Error message if failed */
  error?: string;
  /** Number of retry attempts */
  retryCount: number;
}

/**
 * Queue configuration
 */
export interface QueueConfig {
  /** Maximum retries per suite on failure */
  maxRetries: number;
  /** Continue to next suite on failure (vs stop queue) */
  continueOnFailure: boolean;
  /** Delay between suites (ms) */
  delayBetweenSuites: number;
}

/**
 * Complete queue state
 */
export interface QueueState {
  /** Queue ID */
  id: string;
  /** Queue name */
  name: string;
  /** Creation time */
  createdAt: string;
  /** Last update time */
  lastUpdated: string;
  /** Queue configuration */
  config: QueueConfig;
  /** Queued suites */
  suites: QueuedSuite[];
  /** Current suite index (-1 if not started, suites.length if complete) */
  currentIndex: number;
  /** Overall queue status */
  status: 'pending' | 'running' | 'completed' | 'failed' | 'paused';
}

// =============================================================================
// QUEUE MANAGER
// =============================================================================

const DEFAULT_QUEUE_CONFIG: QueueConfig = {
  maxRetries: 1,
  continueOnFailure: true,
  delayBetweenSuites: 5000,
};

/**
 * Manages a queue of test suites
 */
export class QueueManager {
  private state: QueueState;
  private statePath: string;
  private baseOptions: TunerOptions;

  constructor(
    queueId: string,
    baseOptions: TunerOptions,
    config: Partial<QueueConfig> = {}
  ) {
    this.baseOptions = baseOptions;
    this.statePath = join(baseOptions.outputDir || 'tuning-output', 'queues', `${queueId}.json`);

    // Try to load existing state or create new
    const existingState = this.loadState();
    if (existingState) {
      this.state = existingState;
      console.log(`Resuming queue "${queueId}" from position ${this.state.currentIndex + 1}/${this.state.suites.length}`);
    } else {
      this.state = {
        id: queueId,
        name: queueId,
        createdAt: new Date().toISOString(),
        lastUpdated: new Date().toISOString(),
        config: { ...DEFAULT_QUEUE_CONFIG, ...config },
        suites: [],
        currentIndex: -1,
        status: 'pending',
      };
    }
  }

  /**
   * Load queue state from disk
   */
  private loadState(): QueueState | null {
    if (existsSync(this.statePath)) {
      try {
        return JSON.parse(readFileSync(this.statePath, 'utf-8'));
      } catch {
        console.warn('Failed to load queue state, starting fresh');
      }
    }
    return null;
  }

  /**
   * Save queue state to disk
   */
  private saveState(): void {
    this.state.lastUpdated = new Date().toISOString();
    const dir = join(this.baseOptions.outputDir || 'tuning-output', 'queues');
    if (!existsSync(dir)) {
      const { mkdirSync } = require('fs');
      mkdirSync(dir, { recursive: true });
    }
    writeFileSync(this.statePath, JSON.stringify(this.state, null, 2));
  }

  /**
   * Add a suite to the queue by ID
   */
  addSuite(suiteId: string): boolean {
    const suite = getSuite(suiteId);
    if (!suite) {
      console.error(`Unknown suite: ${suiteId}`);
      return false;
    }

    this.state.suites.push({
      suiteId,
      position: this.state.suites.length,
      status: 'pending',
      retryCount: 0,
    });

    this.saveState();
    return true;
  }

  /**
   * Add multiple suites to the queue
   */
  addSuites(suiteIds: string[]): number {
    let added = 0;
    for (const id of suiteIds) {
      if (this.addSuite(id)) {
        added++;
      }
    }
    return added;
  }

  /**
   * Add a custom suite configuration to the queue
   */
  addCustomSuite(config: SuiteConfig): void {
    // Store custom suite in PREDEFINED_SUITES temporarily
    (PREDEFINED_SUITES as Record<string, SuiteConfig>)[config.id] = config;

    this.state.suites.push({
      suiteId: config.id,
      position: this.state.suites.length,
      status: 'pending',
      retryCount: 0,
    });

    this.saveState();
  }

  /**
   * Run the entire queue
   */
  async run(): Promise<QueueState> {
    console.log('\n' + '='.repeat(70));
    console.log('  TEST QUEUE: ' + this.state.name);
    console.log('='.repeat(70));
    console.log(`  Suites: ${this.state.suites.length}`);
    console.log(`  Continue on failure: ${this.state.config.continueOnFailure}`);
    console.log('='.repeat(70) + '\n');

    this.state.status = 'running';
    this.saveState();

    // Start from current index (for resume) or 0
    const startIndex = Math.max(0, this.state.currentIndex);

    for (let i = startIndex; i < this.state.suites.length; i++) {
      this.state.currentIndex = i;
      const queuedSuite = this.state.suites[i];

      // Skip already completed suites
      if (queuedSuite.status === 'completed') {
        console.log(`\n[${i + 1}/${this.state.suites.length}] ${queuedSuite.suiteId}: Already completed, skipping`);
        continue;
      }

      console.log(`\n[${i + 1}/${this.state.suites.length}] Starting suite: ${queuedSuite.suiteId}`);

      const success = await this.runSuite(queuedSuite);

      if (!success && !this.state.config.continueOnFailure) {
        console.error(`\nQueue stopped due to suite failure: ${queuedSuite.suiteId}`);
        this.state.status = 'failed';
        this.saveState();
        return this.state;
      }

      // Delay between suites
      if (i < this.state.suites.length - 1 && this.state.config.delayBetweenSuites > 0) {
        console.log(`\nWaiting ${this.state.config.delayBetweenSuites / 1000}s before next suite...`);
        await this.delay(this.state.config.delayBetweenSuites);
      }
    }

    this.state.currentIndex = this.state.suites.length;
    this.state.status = 'completed';
    this.saveState();

    this.printSummary();

    return this.state;
  }

  /**
   * Run a single suite with retry logic
   */
  private async runSuite(queuedSuite: QueuedSuite): Promise<boolean> {
    const suite = getSuite(queuedSuite.suiteId);
    if (!suite) {
      queuedSuite.status = 'skipped';
      queuedSuite.error = 'Suite not found';
      this.saveState();
      return false;
    }

    queuedSuite.status = 'running';
    queuedSuite.startedAt = new Date().toISOString();
    this.saveState();

    while (queuedSuite.retryCount <= this.state.config.maxRetries) {
      try {
        const runner = new SuiteRunner(suite, this.baseOptions);
        const result = await runner.run();

        queuedSuite.result = result;

        if (result.phase === 'complete') {
          queuedSuite.status = 'completed';
          queuedSuite.completedAt = new Date().toISOString();
          this.saveState();
          return true;
        } else if (result.phase === 'failed') {
          throw new Error(result.error || 'Suite failed');
        }

      } catch (error) {
        queuedSuite.retryCount++;
        queuedSuite.error = error instanceof Error ? error.message : String(error);

        if (queuedSuite.retryCount <= this.state.config.maxRetries) {
          console.log(`\nRetrying suite (attempt ${queuedSuite.retryCount + 1}/${this.state.config.maxRetries + 1})...`);
          await this.delay(2000);
        }
      }
    }

    queuedSuite.status = 'failed';
    queuedSuite.completedAt = new Date().toISOString();
    this.saveState();
    return false;
  }

  /**
   * Pause the queue at current position
   */
  pause(): void {
    this.state.status = 'paused';
    this.saveState();
    console.log(`Queue paused at position ${this.state.currentIndex + 1}`);
  }

  /**
   * Clear the queue and delete state file
   */
  clear(): void {
    if (existsSync(this.statePath)) {
      unlinkSync(this.statePath);
    }
    this.state.suites = [];
    this.state.currentIndex = -1;
    this.state.status = 'pending';
    console.log('Queue cleared');
  }

  /**
   * Get current queue state
   */
  getState(): QueueState {
    return { ...this.state };
  }

  /**
   * Print queue summary
   */
  printSummary(): void {
    console.log('\n' + '='.repeat(70));
    console.log('  QUEUE SUMMARY');
    console.log('='.repeat(70));

    const completed = this.state.suites.filter(s => s.status === 'completed').length;
    const failed = this.state.suites.filter(s => s.status === 'failed').length;
    const skipped = this.state.suites.filter(s => s.status === 'skipped').length;
    const pending = this.state.suites.filter(s => s.status === 'pending').length;

    console.log(`\n  Results:`);
    console.log(`    Completed: ${completed}/${this.state.suites.length}`);
    if (failed > 0) console.log(`    Failed:    ${failed}`);
    if (skipped > 0) console.log(`    Skipped:   ${skipped}`);
    if (pending > 0) console.log(`    Pending:   ${pending}`);

    console.log('\n  Suite Details:');
    for (const suite of this.state.suites) {
      const icon = suite.status === 'completed' ? '  ' :
                   suite.status === 'failed' ? '  ' :
                   suite.status === 'skipped' ? '  ' : '  ';
      console.log(`    ${icon} ${suite.suiteId}: ${suite.status}`);
      if (suite.error) {
        console.log(`       Error: ${suite.error}`);
      }
      if (suite.result?.progress) {
        const p = suite.result.progress;
        console.log(`       Sweeps: ${p.sweepsCompleted.length} completed`);
      }
    }

    console.log('\n' + '='.repeat(70) + '\n');
  }

  /**
   * Print current queue status
   */
  printStatus(): void {
    console.log('\n  Queue Status:');
    console.log(`    ID: ${this.state.id}`);
    console.log(`    Status: ${this.state.status}`);
    console.log(`    Progress: ${this.state.currentIndex + 1}/${this.state.suites.length}`);
    console.log(`    Created: ${this.state.createdAt}`);
    console.log(`    Last Updated: ${this.state.lastUpdated}`);

    console.log('\n  Suites:');
    for (const suite of this.state.suites) {
      const marker = suite.position === this.state.currentIndex ? '>' : ' ';
      console.log(`    ${marker} [${suite.position + 1}] ${suite.suiteId}: ${suite.status}`);
    }
  }

  private delay(ms: number): Promise<void> {
    return new Promise(resolve => setTimeout(resolve, ms));
  }
}

// =============================================================================
// QUEUE UTILITIES
// =============================================================================

/**
 * List all saved queues
 */
export function listQueues(outputDir: string = 'tuning-output'): string[] {
  const queueDir = join(outputDir, 'queues');
  if (!existsSync(queueDir)) {
    return [];
  }

  const { readdirSync } = require('fs');
  const files: string[] = readdirSync(queueDir);
  return files
    .filter((f: string) => f.endsWith('.json'))
    .map((f: string) => f.replace('.json', ''));
}

/**
 * Load a queue by ID
 */
export function loadQueue(queueId: string, baseOptions: TunerOptions): QueueManager | null {
  const manager = new QueueManager(queueId, baseOptions);
  if (manager.getState().suites.length === 0) {
    return null;
  }
  return manager;
}

/**
 * Create a queue from a list of suite IDs
 */
export function createQueue(
  queueId: string,
  suiteIds: string[],
  baseOptions: TunerOptions,
  config: Partial<QueueConfig> = {}
): QueueManager {
  const manager = new QueueManager(queueId, baseOptions, config);
  manager.addSuites(suiteIds);
  return manager;
}

/**
 * Run multiple suites in sequence (convenience function)
 */
export async function runSuites(
  suiteIds: string[],
  baseOptions: TunerOptions,
  config: Partial<QueueConfig> = {}
): Promise<QueueState> {
  const queueId = `auto-${Date.now()}`;
  const manager = createQueue(queueId, suiteIds, baseOptions, config);
  return manager.run();
}
