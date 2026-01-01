/**
 * State management for resumable tuning sessions
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * All 6 detectors run simultaneously with weighted fusion.
 * Legacy per-mode state tracking has been removed.
 *
 * EXTENSIBILITY: Supports per-pattern incremental saves for:
 * - Interruptible tests (Ctrl+C recovery)
 * - Progress tracking during long-running sweeps
 * - Auto-save at configurable intervals
 */
import { existsSync, mkdirSync, readFileSync, writeFileSync, unlinkSync } from 'fs';
import { join } from 'path';
export class StateManager {
    outputDir;
    statePath;
    state;
    constructor(outputDir) {
        this.outputDir = outputDir;
        this.statePath = join(outputDir, 'state.json');
        // Ensure output directory exists
        this.ensureDir(outputDir);
        this.ensureDir(join(outputDir, 'baseline'));
        this.ensureDir(join(outputDir, 'sweeps'));
        this.ensureDir(join(outputDir, 'incremental')); // Per-pattern saves
        this.ensureDir(join(outputDir, 'interactions'));
        this.ensureDir(join(outputDir, 'validation'));
        this.ensureDir(join(outputDir, 'reports'));
        // Load or create state
        this.state = this.loadState();
    }
    ensureDir(dir) {
        if (!existsSync(dir)) {
            mkdirSync(dir, { recursive: true });
        }
    }
    loadState() {
        if (existsSync(this.statePath)) {
            try {
                return JSON.parse(readFileSync(this.statePath, 'utf-8'));
            }
            catch {
                // If corrupted, start fresh
            }
        }
        return {
            lastUpdated: new Date().toISOString(),
            currentPhase: 'baseline',
            phasesCompleted: [],
            sweeps: {
                completed: [],
                results: {},
            },
            interactions: {
                completed: [],
                results: {},
            },
        };
    }
    save() {
        this.state.lastUpdated = new Date().toISOString();
        writeFileSync(this.statePath, JSON.stringify(this.state, null, 2));
    }
    getState() {
        return this.state;
    }
    hasResumableState() {
        return this.state.phasesCompleted.length > 0 ||
            this.state.baseline !== undefined ||
            (this.state.sweeps?.completed?.length ?? 0) > 0;
    }
    // =============================================================================
    // BASELINE METHODS (Ensemble - single baseline, not per-mode)
    // =============================================================================
    isBaselineComplete() {
        return this.state.phasesCompleted.includes('baseline');
    }
    setBaselineInProgress() {
        this.state.currentPhase = 'baseline';
        this.save();
    }
    saveBaselineResult(result) {
        this.state.baseline = result;
        // Save to file
        writeFileSync(join(this.outputDir, 'baseline', 'ensemble.json'), JSON.stringify(result, null, 2));
        this.save();
    }
    getBaselineResult() {
        return this.state.baseline;
    }
    markBaselinePhaseComplete() {
        if (!this.state.phasesCompleted.includes('baseline')) {
            this.state.phasesCompleted.push('baseline');
        }
        this.save();
    }
    // =============================================================================
    // SWEEP METHODS
    // =============================================================================
    isSweepComplete(param) {
        return this.state.sweeps?.completed?.includes(param) ?? false;
    }
    setSweepInProgress(param, index) {
        if (!this.state.sweeps) {
            this.state.sweeps = { completed: [], results: {} };
        }
        this.state.sweeps.current = param;
        this.state.sweeps.currentIndex = index;
        this.state.currentPhase = 'sweep';
        this.save();
    }
    saveSweepResult(param, result) {
        if (!this.state.sweeps) {
            this.state.sweeps = { completed: [], results: {} };
        }
        this.state.sweeps.results[param] = result;
        if (!this.state.sweeps.completed.includes(param)) {
            this.state.sweeps.completed.push(param);
        }
        delete this.state.sweeps.current;
        delete this.state.sweeps.currentIndex;
        // Save to file
        writeFileSync(join(this.outputDir, 'sweeps', `${param}.json`), JSON.stringify(result, null, 2));
        this.save();
    }
    getSweepResult(param) {
        return this.state.sweeps?.results[param];
    }
    getSweepResumeIndex(param) {
        if (this.state.sweeps?.current === param && this.state.sweeps.currentIndex !== undefined) {
            return this.state.sweeps.currentIndex;
        }
        return 0;
    }
    markSweepPhaseComplete() {
        if (!this.state.phasesCompleted.includes('sweep')) {
            this.state.phasesCompleted.push('sweep');
        }
        this.save();
    }
    // =============================================================================
    // INCREMENTAL SWEEP METHODS - Per-pattern saves for interruptible tests
    // =============================================================================
    /**
     * Get path for incremental sweep file
     */
    getIncrementalPath(param) {
        return join(this.outputDir, 'incremental', `${param}.json`);
    }
    /**
     * Start or resume a sweep with incremental saves
     * Returns the progress to resume from (or fresh start)
     */
    getIncrementalSweepProgress(param) {
        const path = this.getIncrementalPath(param);
        if (existsSync(path)) {
            try {
                return JSON.parse(readFileSync(path, 'utf-8'));
            }
            catch {
                // Corrupted, start fresh
            }
        }
        return null;
    }
    /**
     * Save progress after each pattern test completes
     * Called after each individual pattern in a sweep
     */
    saveIncrementalProgress(progress) {
        const path = this.getIncrementalPath(progress.parameter);
        writeFileSync(path, JSON.stringify(progress, null, 2));
        // Also update the main state with current position
        if (!this.state.sweeps) {
            this.state.sweeps = { completed: [], results: {} };
        }
        this.state.sweeps.current = progress.parameter;
        this.state.sweeps.currentIndex = progress.valueIndex;
        this.state.currentPhase = 'sweep';
        this.save();
    }
    /**
     * Clear incremental progress after sweep completes
     */
    clearIncrementalProgress(param) {
        const path = this.getIncrementalPath(param);
        if (existsSync(path)) {
            try {
                unlinkSync(path);
            }
            catch {
                // Ignore deletion errors
            }
        }
    }
    /**
     * Save a single pattern result to the incremental file
     * This allows recovery even if interrupted mid-pattern-set
     */
    appendPatternResult(param, valueIndex, value, patternResult) {
        let progress = this.getIncrementalSweepProgress(param);
        if (!progress || progress.valueIndex !== valueIndex) {
            // Starting a new value, reset current value results
            progress = {
                parameter: param,
                valueIndex,
                patternIndex: 0,
                currentValue: value,
                partialResults: progress?.partialResults || [],
                currentValueResults: [],
            };
        }
        // Add the pattern result
        progress.currentValueResults.push(patternResult);
        progress.patternIndex++;
        this.saveIncrementalProgress(progress);
    }
    /**
     * Finalize a sweep value (all patterns tested for this value)
     * Adds to partialResults and resets currentValueResults
     */
    finalizeSweepValue(param, sweepPoint) {
        let progress = this.getIncrementalSweepProgress(param);
        if (!progress) {
            progress = {
                parameter: param,
                valueIndex: 0,
                patternIndex: 0,
                currentValue: sweepPoint.value,
                partialResults: [],
                currentValueResults: [],
            };
        }
        progress.partialResults.push(sweepPoint);
        progress.valueIndex++;
        progress.patternIndex = 0;
        progress.currentValueResults = [];
        this.saveIncrementalProgress(progress);
    }
    /**
     * Check if a specific pattern in a sweep has already been completed
     * Used to skip already-tested patterns when resuming
     */
    isPatternCompletedInSweep(param, valueIndex, patternIndex) {
        const progress = this.getIncrementalSweepProgress(param);
        if (!progress)
            return false;
        // If we're past this value, it's completed
        if (progress.valueIndex > valueIndex)
            return true;
        // If we're at this value, check pattern index
        if (progress.valueIndex === valueIndex) {
            return progress.patternIndex > patternIndex;
        }
        return false;
    }
    /**
     * Get partial results for a param (for resuming)
     */
    getPartialSweepResults(param) {
        const progress = this.getIncrementalSweepProgress(param);
        return progress?.partialResults || [];
    }
    // =============================================================================
    // INCREMENTAL BASELINE METHODS - Per-pattern saves for interruptible baseline tests
    // =============================================================================
    /**
     * Get path for incremental baseline file
     */
    getIncrementalBaselinePath() {
        return join(this.outputDir, 'incremental', 'baseline-ensemble.json');
    }
    /**
     * Get incremental baseline progress
     */
    getIncrementalBaselineProgress() {
        const path = this.getIncrementalBaselinePath();
        if (existsSync(path)) {
            try {
                return JSON.parse(readFileSync(path, 'utf-8'));
            }
            catch {
                // Corrupted, start fresh
            }
        }
        return null;
    }
    /**
     * Save baseline progress after each pattern completes
     */
    saveIncrementalBaselineProgress(progress) {
        const path = this.getIncrementalBaselinePath();
        writeFileSync(path, JSON.stringify(progress, null, 2));
        // Update main state
        this.state.currentPhase = 'baseline';
        this.save();
    }
    /**
     * Append a pattern result to baseline progress
     */
    appendBaselinePatternResult(pattern, result) {
        let progress = this.getIncrementalBaselineProgress();
        if (!progress) {
            progress = {
                patternIndex: 0,
                completedPatterns: [],
                partialResults: {},
            };
        }
        progress.partialResults[pattern] = result;
        progress.completedPatterns.push(pattern);
        progress.patternIndex = progress.completedPatterns.length;
        this.saveIncrementalBaselineProgress(progress);
    }
    /**
     * Check if a pattern has been completed in baseline
     */
    isBaselinePatternCompleted(pattern) {
        const progress = this.getIncrementalBaselineProgress();
        return progress?.completedPatterns?.includes(pattern) ?? false;
    }
    /**
     * Get partial baseline results for resuming
     */
    getPartialBaselineResults() {
        const progress = this.getIncrementalBaselineProgress();
        return progress?.partialResults || {};
    }
    /**
     * Clear incremental baseline progress after baseline completes
     */
    clearIncrementalBaselineProgress() {
        const path = this.getIncrementalBaselinePath();
        if (existsSync(path)) {
            try {
                unlinkSync(path);
            }
            catch {
                // Ignore deletion errors
            }
        }
    }
    // =============================================================================
    // INTERACTION METHODS
    // =============================================================================
    isInteractionComplete(name) {
        return this.state.interactions?.completed?.includes(name) ?? false;
    }
    setInteractionInProgress(name, index) {
        if (!this.state.interactions) {
            this.state.interactions = { completed: [], results: {} };
        }
        this.state.interactions.current = name;
        this.state.interactions.currentIndex = index;
        this.state.currentPhase = 'interact';
        this.save();
    }
    saveInteractionResult(name, result) {
        if (!this.state.interactions) {
            this.state.interactions = { completed: [], results: {} };
        }
        this.state.interactions.results[name] = result;
        if (!this.state.interactions.completed.includes(name)) {
            this.state.interactions.completed.push(name);
        }
        delete this.state.interactions.current;
        delete this.state.interactions.currentIndex;
        // Save to file
        writeFileSync(join(this.outputDir, 'interactions', `${name}.json`), JSON.stringify(result, null, 2));
        this.save();
    }
    getInteractionResult(name) {
        return this.state.interactions?.results[name];
    }
    getInteractionResumeIndex(name) {
        if (this.state.interactions?.current === name && this.state.interactions.currentIndex !== undefined) {
            return this.state.interactions.currentIndex;
        }
        return 0;
    }
    markInteractionPhaseComplete() {
        if (!this.state.phasesCompleted.includes('interact')) {
            this.state.phasesCompleted.push('interact');
        }
        this.save();
    }
    // =============================================================================
    // VALIDATION METHODS (Ensemble - single validation, not per-mode)
    // =============================================================================
    isValidationComplete() {
        return this.state.phasesCompleted.includes('validate');
    }
    setValidationInProgress() {
        this.state.currentPhase = 'validate';
        this.save();
    }
    saveValidationResult(result) {
        this.state.validation = result;
        // Save to file
        writeFileSync(join(this.outputDir, 'validation', 'ensemble.json'), JSON.stringify(result, null, 2));
        this.save();
    }
    getValidationResult() {
        return this.state.validation;
    }
    markValidationPhaseComplete() {
        if (!this.state.phasesCompleted.includes('validate')) {
            this.state.phasesCompleted.push('validate');
        }
        this.save();
    }
    // =============================================================================
    // OPTIMAL PARAMETERS
    // =============================================================================
    setOptimalParams(params) {
        this.state.optimalParams = params;
        this.save();
    }
    getOptimalParams() {
        return this.state.optimalParams;
    }
    // =============================================================================
    // COMPLETE / RESET
    // =============================================================================
    markDone() {
        this.state.currentPhase = 'done';
        if (!this.state.phasesCompleted.includes('report')) {
            this.state.phasesCompleted.push('report');
        }
        this.save();
    }
    reset() {
        this.state = {
            lastUpdated: new Date().toISOString(),
            currentPhase: 'baseline',
            phasesCompleted: [],
            sweeps: {
                completed: [],
                results: {},
            },
            interactions: {
                completed: [],
                results: {},
            },
        };
        this.save();
    }
}
