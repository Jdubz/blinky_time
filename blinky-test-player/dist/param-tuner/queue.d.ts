/**
 * Test Queue Management System
 *
 * Enables scheduling multiple test suites in sequence with:
 * - Persistent queue state for resume after interruption
 * - Progress tracking across suites
 * - Automatic retry on failure
 * - Summary report after all suites complete
 */
import type { TunerOptions } from './types.js';
import type { SuiteConfig, SuiteStatus } from './suite.js';
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
    /** Custom suite configurations (stored in queue, not in global registry) */
    customSuites?: Record<string, SuiteConfig>;
}
/**
 * Manages a queue of test suites
 */
export declare class QueueManager {
    private state;
    private statePath;
    private baseOptions;
    constructor(queueId: string, baseOptions: TunerOptions, config?: Partial<QueueConfig>);
    /**
     * Load queue state from disk
     */
    private loadState;
    /**
     * Save queue state to disk
     */
    private saveState;
    /**
     * Add a suite to the queue by ID
     */
    addSuite(suiteId: string): boolean;
    /**
     * Add multiple suites to the queue
     */
    addSuites(suiteIds: string[]): number;
    /**
     * Add a custom suite configuration to the queue
     */
    addCustomSuite(config: SuiteConfig): void;
    /**
     * Get suite config by ID (checks predefined first, then custom)
     */
    private getSuiteConfig;
    /**
     * Run the entire queue
     */
    run(): Promise<QueueState>;
    /**
     * Run a single suite with retry logic
     */
    private runSuite;
    /**
     * Pause the queue at current position
     */
    pause(): void;
    /**
     * Clear the queue and delete state file
     */
    clear(): void;
    /**
     * Get current queue state
     */
    getState(): QueueState;
    /**
     * Print queue summary
     */
    printSummary(): void;
    /**
     * Print current queue status
     */
    printStatus(): void;
    private delay;
}
/**
 * List all saved queues
 */
export declare function listQueues(outputDir?: string): string[];
/**
 * Load a queue by ID
 */
export declare function loadQueue(queueId: string, baseOptions: TunerOptions): QueueManager | null;
/**
 * Create a queue from a list of suite IDs
 */
export declare function createQueue(queueId: string, suiteIds: string[], baseOptions: TunerOptions, config?: Partial<QueueConfig>): QueueManager;
/**
 * Run multiple suites in sequence (convenience function)
 */
export declare function runSuites(suiteIds: string[], baseOptions: TunerOptions, config?: Partial<QueueConfig>): Promise<QueueState>;
