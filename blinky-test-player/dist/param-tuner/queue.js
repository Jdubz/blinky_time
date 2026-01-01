/**
 * Test Queue Management System
 *
 * Enables scheduling multiple test suites in sequence with:
 * - Persistent queue state for resume after interruption
 * - Progress tracking across suites
 * - Automatic retry on failure
 * - Summary report after all suites complete
 */
import { existsSync, readFileSync, writeFileSync, unlinkSync, mkdirSync, readdirSync } from 'fs';
import { join } from 'path';
import { SuiteRunner, getSuite } from './suite.js';
// =============================================================================
// QUEUE MANAGER
// =============================================================================
const DEFAULT_QUEUE_CONFIG = {
    maxRetries: 1,
    continueOnFailure: true,
    delayBetweenSuites: 5000,
};
/**
 * Manages a queue of test suites
 */
export class QueueManager {
    state;
    statePath;
    baseOptions;
    constructor(queueId, baseOptions, config = {}) {
        this.baseOptions = baseOptions;
        this.statePath = join(baseOptions.outputDir || 'tuning-output', 'queues', `${queueId}.json`);
        // Try to load existing state or create new
        const existingState = this.loadState();
        if (existingState) {
            this.state = existingState;
            console.log(`Resuming queue "${queueId}" from position ${this.state.currentIndex + 1}/${this.state.suites.length}`);
        }
        else {
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
    loadState() {
        if (existsSync(this.statePath)) {
            try {
                return JSON.parse(readFileSync(this.statePath, 'utf-8'));
            }
            catch {
                console.warn('Failed to load queue state, starting fresh');
            }
        }
        return null;
    }
    /**
     * Save queue state to disk
     */
    saveState() {
        this.state.lastUpdated = new Date().toISOString();
        const dir = join(this.baseOptions.outputDir || 'tuning-output', 'queues');
        if (!existsSync(dir)) {
            mkdirSync(dir, { recursive: true });
        }
        writeFileSync(this.statePath, JSON.stringify(this.state, null, 2));
    }
    /**
     * Add a suite to the queue by ID
     */
    addSuite(suiteId) {
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
    addSuites(suiteIds) {
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
    addCustomSuite(config) {
        // Store custom suite in queue state (not in global registry)
        if (!this.state.customSuites) {
            this.state.customSuites = {};
        }
        this.state.customSuites[config.id] = config;
        this.state.suites.push({
            suiteId: config.id,
            position: this.state.suites.length,
            status: 'pending',
            retryCount: 0,
        });
        this.saveState();
    }
    /**
     * Get suite config by ID (checks predefined first, then custom)
     */
    getSuiteConfig(suiteId) {
        return getSuite(suiteId) || this.state.customSuites?.[suiteId];
    }
    /**
     * Run the entire queue
     */
    async run() {
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
    async runSuite(queuedSuite) {
        const suite = this.getSuiteConfig(queuedSuite.suiteId);
        if (!suite) {
            queuedSuite.status = 'skipped';
            queuedSuite.error = `Suite not found: ${queuedSuite.suiteId}`;
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
                }
                else if (result.phase === 'failed') {
                    throw new Error(result.error || 'Suite failed');
                }
            }
            catch (error) {
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
    pause() {
        this.state.status = 'paused';
        this.saveState();
        console.log(`Queue paused at position ${this.state.currentIndex + 1}`);
    }
    /**
     * Clear the queue and delete state file
     */
    clear() {
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
    getState() {
        return { ...this.state };
    }
    /**
     * Print queue summary
     */
    printSummary() {
        console.log('\n' + '='.repeat(70));
        console.log('  QUEUE SUMMARY');
        console.log('='.repeat(70));
        const completed = this.state.suites.filter(s => s.status === 'completed').length;
        const failed = this.state.suites.filter(s => s.status === 'failed').length;
        const skipped = this.state.suites.filter(s => s.status === 'skipped').length;
        const pending = this.state.suites.filter(s => s.status === 'pending').length;
        console.log(`\n  Results:`);
        console.log(`    Completed: ${completed}/${this.state.suites.length}`);
        if (failed > 0)
            console.log(`    Failed:    ${failed}`);
        if (skipped > 0)
            console.log(`    Skipped:   ${skipped}`);
        if (pending > 0)
            console.log(`    Pending:   ${pending}`);
        console.log('\n  Suite Details:');
        for (const suite of this.state.suites) {
            const icon = suite.status === 'completed' ? '✅' :
                suite.status === 'failed' ? '❌' :
                    suite.status === 'skipped' ? '⚠️' : '⏳';
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
    printStatus() {
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
    delay(ms) {
        return new Promise(resolve => setTimeout(resolve, ms));
    }
}
// =============================================================================
// QUEUE UTILITIES
// =============================================================================
/**
 * List all saved queues
 */
export function listQueues(outputDir = 'tuning-output') {
    const queueDir = join(outputDir, 'queues');
    if (!existsSync(queueDir)) {
        return [];
    }
    const files = readdirSync(queueDir);
    return files
        .filter((f) => f.endsWith('.json'))
        .map((f) => f.replace('.json', ''));
}
/**
 * Load a queue by ID
 */
export function loadQueue(queueId, baseOptions) {
    const manager = new QueueManager(queueId, baseOptions);
    if (manager.getState().suites.length === 0) {
        return null;
    }
    return manager;
}
/**
 * Create a queue from a list of suite IDs
 */
export function createQueue(queueId, suiteIds, baseOptions, config = {}) {
    const manager = new QueueManager(queueId, baseOptions, config);
    manager.addSuites(suiteIds);
    return manager;
}
/**
 * Run multiple suites in sequence (convenience function)
 */
export async function runSuites(suiteIds, baseOptions, config = {}) {
    const queueId = `auto-${Date.now()}`;
    const manager = createQueue(queueId, suiteIds, baseOptions, config);
    return manager.run();
}
