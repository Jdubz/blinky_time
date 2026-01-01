/**
 * Calibration Test Suite
 * Orchestrates the full calibration workflow
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * Single ensemble-based calibration workflow.
 * Legacy per-mode testing has been removed.
 *
 * Workflow:
 * 1. Baseline - Establish baseline performance with defaults
 * 2. Sweep - Find optimal value for each parameter
 * 3. Interact - Test parameter interactions
 * 4. Validate - Validate optimal parameters on all patterns
 * 5. Report - Generate summary report
 */
import { PARAMETERS, REPRESENTATIVE_PATTERNS, ALL_PATTERNS } from './types.js';
import { StateManager } from './state.js';
import { runBaseline, showBaselineSummary } from './baseline.js';
import { runSweeps, showSweepSummary } from './sweep.js';
import { runInteractions, showInteractionSummary } from './interact.js';
import { runValidation, showValidationSummary } from './validate.js';
import { generateReport } from './report.js';
import { saveOptimalToDevice, showOptimalParams } from './device-save.js';
const SUITE_PHASES = [
    {
        name: 'baseline',
        description: 'Establish baseline performance with default parameters',
        run: runBaseline,
    },
    {
        name: 'sweep',
        description: 'Sweep each parameter to find optimal values',
        run: runSweeps,
    },
    {
        name: 'interact',
        description: 'Test parameter interactions',
        run: runInteractions,
    },
    {
        name: 'validate',
        description: 'Validate optimal parameters on all patterns',
        run: runValidation,
    },
    {
        name: 'report',
        description: 'Generate summary report',
        run: generateReport,
    },
];
// =============================================================================
// MAIN SUITE FUNCTIONS
// =============================================================================
/**
 * Run the full calibration suite
 */
export async function runSuite(options, config = {}) {
    console.log('\n Ensemble Calibration Suite');
    console.log('='.repeat(50));
    console.log('Running full calibration workflow for ensemble detection.\n');
    const outputDir = options.outputDir || 'tuning-results';
    const stateManager = new StateManager(outputDir);
    // Check for resumable state
    if (config.resume && stateManager.hasResumableState()) {
        console.log('Resuming from previous state...\n');
    }
    else if (!config.resume && stateManager.hasResumableState()) {
        console.log('Previous state found. Use --resume to continue or --reset to start fresh.\n');
        console.log('Starting fresh...');
        stateManager.reset();
    }
    // Determine which phases to run
    const phasesToRun = config.phases
        ? SUITE_PHASES.filter(p => config.phases.includes(p.name))
        : SUITE_PHASES;
    // Apply config to options
    const suiteOptions = {
        ...options,
        params: config.params,
        patterns: config.patterns,
    };
    // Run phases
    for (const phase of phasesToRun) {
        console.log(`\n${'='.repeat(50)}`);
        console.log(`  ${phase.name.toUpperCase()}`);
        console.log(`  ${phase.description}`);
        console.log('='.repeat(50));
        try {
            await phase.run(suiteOptions, stateManager);
        }
        catch (err) {
            console.error(`\nError in ${phase.name} phase:`, err);
            console.log('State has been saved. Re-run with --resume to continue.');
            throw err;
        }
    }
    // Save to device if requested
    if (config.saveToDevice) {
        await saveOptimalToDevice(options, stateManager);
    }
    console.log('\n Calibration Complete!');
    console.log('='.repeat(50));
}
/**
 * Show summary of current state
 */
export async function showSuiteSummary(outputDir) {
    const stateManager = new StateManager(outputDir);
    const state = stateManager.getState();
    console.log('\n Calibration Status');
    console.log('='.repeat(50));
    console.log(`Last updated: ${state.lastUpdated}`);
    console.log(`Current phase: ${state.currentPhase}`);
    console.log(`Completed phases: ${state.phasesCompleted.join(', ') || 'none'}`);
    if (state.baseline) {
        await showBaselineSummary(stateManager);
    }
    if (state.sweeps?.completed?.length) {
        await showSweepSummary(stateManager);
    }
    if (state.interactions?.completed?.length) {
        await showInteractionSummary(stateManager);
    }
    if (state.validation) {
        await showValidationSummary(stateManager);
    }
    if (state.optimalParams) {
        await showOptimalParams(stateManager);
    }
}
/**
 * Quick sweep of a specific parameter
 */
export async function quickSweep(options, paramName) {
    const param = PARAMETERS[paramName];
    if (!param) {
        console.error(`Unknown parameter: ${paramName}`);
        return;
    }
    console.log(`\n Quick Sweep: ${paramName}`);
    console.log('='.repeat(50));
    const tempOutputDir = `tuning-results/quick-${paramName}`;
    const stateManager = new StateManager(tempOutputDir);
    await runSweeps({
        ...options,
        params: [paramName],
        patterns: [...REPRESENTATIVE_PATTERNS.slice(0, 4)], // Quick: just 4 patterns
        outputDir: tempOutputDir,
    }, stateManager);
    await showSweepSummary(stateManager);
}
/**
 * Validate current device settings
 */
export async function validateCurrentSettings(options) {
    console.log('\n Validating Current Device Settings');
    console.log('='.repeat(50));
    const tempOutputDir = 'tuning-results/validate-current';
    const stateManager = new StateManager(tempOutputDir);
    stateManager.reset();
    // Just run validation without baseline/sweep
    await runValidation({
        ...options,
        patterns: [...REPRESENTATIVE_PATTERNS],
        outputDir: tempOutputDir,
    }, stateManager);
}
/**
 * Get parameter suggestions based on pattern performance
 */
export function getParameterSuggestions(patternId) {
    const suggestions = [];
    for (const param of Object.values(PARAMETERS)) {
        if (param.targetPatterns?.includes(patternId)) {
            suggestions.push(param);
        }
    }
    return suggestions;
}
/**
 * List all available patterns
 */
export function listPatterns() {
    console.log('\n Available Test Patterns');
    console.log('='.repeat(50));
    console.log('\nRepresentative Patterns (quick tests):');
    for (const pattern of REPRESENTATIVE_PATTERNS) {
        console.log(`  - ${pattern}`);
    }
    console.log('\nAll Patterns:');
    for (const pattern of ALL_PATTERNS) {
        console.log(`  - ${pattern}`);
    }
}
/**
 * List all tunable parameters
 */
export function listParameters() {
    console.log('\n Tunable Parameters');
    console.log('='.repeat(50));
    const byMode = new Map();
    for (const param of Object.values(PARAMETERS)) {
        if (!byMode.has(param.mode)) {
            byMode.set(param.mode, []);
        }
        byMode.get(param.mode).push(param);
    }
    for (const [mode, params] of byMode) {
        console.log(`\n${mode.toUpperCase()}:`);
        for (const param of params) {
            console.log(`  ${param.name}: ${param.description}`);
            console.log(`    Range: ${param.min} - ${param.max}, Default: ${param.default}`);
        }
    }
}
// =============================================================================
// PREDEFINED SUITES
// =============================================================================
/**
 * Predefined test suites for common calibration workflows
 */
export const PREDEFINED_SUITES = {
    'full': {
        id: 'full',
        name: 'Full Calibration',
        description: 'Complete ensemble calibration with all phases',
        phases: ['baseline', 'sweep', 'interact', 'validate', 'report'],
    },
    'quick': {
        id: 'quick',
        name: 'Quick Tune',
        description: 'Fast calibration with representative patterns only',
        phases: ['baseline', 'sweep', 'validate'],
        patterns: [...REPRESENTATIVE_PATTERNS.slice(0, 4)],
    },
    'thresholds': {
        id: 'thresholds',
        name: 'Threshold Tuning',
        description: 'Sweep detector thresholds only',
        phases: ['sweep', 'validate'],
        params: ['drummer_thresh', 'spectral_thresh', 'hfc_thresh', 'bass_thresh'],
    },
    'weights': {
        id: 'weights',
        name: 'Weight Tuning',
        description: 'Sweep detector weights only',
        phases: ['sweep', 'interact', 'validate'],
        params: ['drummer_weight', 'spectral_weight', 'hfc_weight', 'bass_weight'],
    },
    'agreement': {
        id: 'agreement',
        name: 'Agreement Tuning',
        description: 'Tune agreement boost values',
        phases: ['sweep', 'interact', 'validate'],
        params: ['agree_1', 'agree_2', 'agree_3'],
    },
    'validate-only': {
        id: 'validate-only',
        name: 'Validation Only',
        description: 'Validate current settings without modification',
        phases: ['validate'],
    },
};
// =============================================================================
// SUITE RUNNER CLASS
// =============================================================================
/**
 * Runs a suite configuration through its phases
 */
export class SuiteRunner {
    suite;
    options;
    stateManager;
    status;
    constructor(suite, options) {
        this.suite = suite;
        this.options = {
            ...options,
            params: suite.params || options.params,
            patterns: suite.patterns || options.patterns,
        };
        const outputDir = options.outputDir || `tuning-results/${suite.id}`;
        this.stateManager = new StateManager(outputDir);
        this.status = {
            phase: 'pending',
            progress: {
                sweepsCompleted: [],
                interactionsCompleted: [],
                currentPhase: 'pending',
            },
        };
    }
    /**
     * Run the suite
     */
    async run() {
        console.log(`\n Running Suite: ${this.suite.name}`);
        console.log('='.repeat(50));
        if (this.suite.description) {
            console.log(this.suite.description);
        }
        console.log('');
        const phases = this.suite.phases || ['baseline', 'sweep', 'interact', 'validate', 'report'];
        try {
            for (const phaseName of phases) {
                this.status.phase = phaseName;
                this.status.progress.currentPhase = phaseName;
                const phase = SUITE_PHASES.find(p => p.name === phaseName);
                if (!phase) {
                    console.warn(`Unknown phase: ${phaseName}, skipping`);
                    continue;
                }
                console.log(`\n${'='.repeat(50)}`);
                console.log(`  ${phase.name.toUpperCase()}`);
                console.log(`  ${phase.description}`);
                console.log('='.repeat(50));
                await phase.run(this.options, this.stateManager);
                // Track completed sweeps
                if (phaseName === 'sweep') {
                    const state = this.stateManager.getState();
                    this.status.progress.sweepsCompleted = state.sweeps?.completed || [];
                }
                if (phaseName === 'interact') {
                    const state = this.stateManager.getState();
                    this.status.progress.interactionsCompleted = state.interactions?.completed || [];
                }
            }
            // Save to device if configured
            if (this.suite.saveToDevice) {
                await saveOptimalToDevice(this.options, this.stateManager);
            }
            this.status.phase = 'complete';
            this.status.progress.currentPhase = 'complete';
            console.log('\n Suite Complete!');
        }
        catch (err) {
            this.status.phase = 'failed';
            this.status.error = err instanceof Error ? err.message : String(err);
            console.error(`\nSuite failed: ${this.status.error}`);
        }
        return this.status;
    }
    /**
     * Get the state manager
     */
    getStateManager() {
        return this.stateManager;
    }
    /**
     * Get current status
     */
    getStatus() {
        return { ...this.status };
    }
}
// =============================================================================
// SUITE UTILITIES
// =============================================================================
/**
 * Get a predefined suite by ID
 */
export function getSuite(id) {
    return PREDEFINED_SUITES[id];
}
/**
 * List all available suites
 */
export function listSuites() {
    console.log('\n Available Test Suites');
    console.log('='.repeat(50));
    for (const [id, suite] of Object.entries(PREDEFINED_SUITES)) {
        console.log(`\n  ${id}: ${suite.name}`);
        if (suite.description) {
            console.log(`    ${suite.description}`);
        }
        if (suite.phases) {
            console.log(`    Phases: ${suite.phases.join(', ')}`);
        }
        if (suite.params) {
            console.log(`    Params: ${suite.params.join(', ')}`);
        }
    }
}
/**
 * Validate a suite configuration
 */
export function validateSuiteConfig(config) {
    const errors = [];
    if (!config.id) {
        errors.push('Suite must have an id');
    }
    if (!config.name) {
        errors.push('Suite must have a name');
    }
    // Validate phases
    if (config.phases) {
        const validPhases = ['baseline', 'sweep', 'interact', 'validate', 'report'];
        for (const phase of config.phases) {
            if (!validPhases.includes(phase)) {
                errors.push(`Unknown phase: ${phase}`);
            }
        }
    }
    // Validate params
    if (config.params) {
        for (const param of config.params) {
            if (!PARAMETERS[param]) {
                errors.push(`Unknown parameter: ${param}`);
            }
        }
    }
    // Validate sweeps
    if (config.sweeps) {
        for (const sweep of config.sweeps) {
            if (!sweep.parameter) {
                errors.push('Sweep must have a parameter');
            }
            else if (!PARAMETERS[sweep.parameter]) {
                errors.push(`Unknown sweep parameter: ${sweep.parameter}`);
            }
        }
    }
    return errors;
}
