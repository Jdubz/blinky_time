/**
 * State management for resumable tuning sessions
 */
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'fs';
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
            baseline: {
                completed: [],
                results: {},
            },
            sweeps: {
                completed: [],
                results: {},
            },
            interactions: {
                completed: [],
                results: {},
            },
            validation: {
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
            (this.state.baseline?.completed?.length ?? 0) > 0 ||
            (this.state.sweeps?.completed?.length ?? 0) > 0;
    }
    // Baseline methods
    isBaselineComplete(mode) {
        return this.state.baseline?.completed?.includes(mode) ?? false;
    }
    setBaselineInProgress(mode) {
        if (!this.state.baseline) {
            this.state.baseline = { completed: [], results: {} };
        }
        this.state.baseline.current = mode;
        this.state.currentPhase = 'baseline';
        this.save();
    }
    saveBaselineResult(mode, result) {
        if (!this.state.baseline) {
            this.state.baseline = { completed: [], results: {} };
        }
        this.state.baseline.results[mode] = result;
        if (!this.state.baseline.completed.includes(mode)) {
            this.state.baseline.completed.push(mode);
        }
        delete this.state.baseline.current;
        // Save to file
        writeFileSync(join(this.outputDir, 'baseline', `${mode}.json`), JSON.stringify(result, null, 2));
        this.save();
    }
    getBaselineResult(mode) {
        return this.state.baseline?.results[mode];
    }
    markBaselinePhaseComplete() {
        if (!this.state.phasesCompleted.includes('baseline')) {
            this.state.phasesCompleted.push('baseline');
        }
        this.save();
    }
    // Sweep methods
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
    // Interaction methods
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
    // Validation methods
    isValidationComplete(mode) {
        return this.state.validation?.completed?.includes(mode) ?? false;
    }
    setValidationInProgress(mode) {
        if (!this.state.validation) {
            this.state.validation = { completed: [], results: {} };
        }
        this.state.validation.current = mode;
        this.state.currentPhase = 'validate';
        this.save();
    }
    saveValidationResult(mode, result) {
        if (!this.state.validation) {
            this.state.validation = { completed: [], results: {} };
        }
        this.state.validation.results[mode] = result;
        if (!this.state.validation.completed.includes(mode)) {
            this.state.validation.completed.push(mode);
        }
        delete this.state.validation.current;
        // Save to file
        writeFileSync(join(this.outputDir, 'validation', `${mode}.json`), JSON.stringify(result, null, 2));
        this.save();
    }
    getValidationResult(mode) {
        return this.state.validation?.results[mode];
    }
    markValidationPhaseComplete() {
        if (!this.state.phasesCompleted.includes('validate')) {
            this.state.phasesCompleted.push('validate');
        }
        this.save();
    }
    // Optimal parameters
    setOptimalParams(mode, params) {
        if (!this.state.optimalParams) {
            this.state.optimalParams = {
                drummer: {},
                spectral: {},
                hybrid: {},
            };
        }
        this.state.optimalParams[mode] = params;
        this.save();
    }
    getOptimalParams(mode) {
        return this.state.optimalParams?.[mode];
    }
    // Complete marking
    markDone() {
        this.state.currentPhase = 'done';
        if (!this.state.phasesCompleted.includes('report')) {
            this.state.phasesCompleted.push('report');
        }
        this.save();
    }
    // Reset
    reset() {
        this.state = {
            lastUpdated: new Date().toISOString(),
            currentPhase: 'baseline',
            phasesCompleted: [],
            baseline: {
                completed: [],
                results: {},
            },
            sweeps: {
                completed: [],
                results: {},
            },
            interactions: {
                completed: [],
                results: {},
            },
            validation: {
                completed: [],
                results: {},
            },
        };
        this.save();
    }
}
