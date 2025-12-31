/**
 * Test Suite Configuration System
 *
 * Enables configurable test suites with:
 * - Baseline calibration before sweeps
 * - Targeted parameter sweeps with specific patterns
 * - Validation with optimal parameters
 * - Per-pattern result recording for interruptible tests
 *
 * EXTENSIBILITY: Adding a new suite requires only creating a SuiteConfig object.
 * The system auto-discovers patterns based on category and parameter targeting.
 */

import type { DetectionMode, ParameterMode, TunerOptions, ParameterDef } from './types.js';
import { PARAMETERS, DETECTION_MODES } from './types.js';
import { StateManager } from './state.js';
import { TestRunner } from './runner.js';
import { runBaseline } from './baseline.js';
import { runSweeps } from './sweep.js';
import { runValidation } from './validate.js';
import { PATTERN_REGISTRY, getPatternsForParam, getPatternsByCategory } from '../patterns.js';
import { analyzeBoundaries, printBoundaryReport } from './boundary-analyzer.js';
import { saveOptimizedToDevice, printDeviceSaveReport } from './device-save.js';

// =============================================================================
// SUITE CONFIGURATION TYPES
// =============================================================================

/**
 * Baseline phase configuration
 */
export interface BaselineConfig {
  /** Detection modes to baseline (default: all) */
  modes?: DetectionMode[];
  /** Specific patterns to use (default: auto-select based on mode) */
  patterns?: string[];
  /** Number of repetitions per pattern (default: 3) */
  repetitions?: number;
}

/**
 * Single parameter sweep configuration
 */
export interface SweepConfig {
  /** Parameter name to sweep */
  parameter: string;
  /** Override default sweep values */
  values?: number[];
  /** Patterns to use for this sweep (default: auto-select by param) */
  patterns?: string[];
  /** Modes to test (default: parameter's mode) */
  modes?: DetectionMode[];
}

/**
 * Validation phase configuration
 */
export interface ValidationConfig {
  /** Detection modes to validate */
  modes?: DetectionMode[];
  /** Patterns to validate (default: all) */
  patterns?: string[];
  /** Use optimized parameters from sweeps (default: true) */
  useOptimized?: boolean;
}

/**
 * Complete test suite configuration
 */
export interface SuiteConfig {
  /** Unique suite identifier (kebab-case) */
  id: string;
  /** Human-readable suite name */
  name: string;
  /** Suite description */
  description: string;

  /** Baseline phase configuration (optional) */
  baseline?: BaselineConfig;

  /** Parameter sweeps to run */
  sweeps?: SweepConfig[];

  /** Validation phase configuration (optional) */
  validation?: ValidationConfig;

  /** Result save interval */
  saveInterval: 'per-pattern' | 'per-sweep-value' | 'per-phase';

  /** Custom output directory (default: tuning-output/<suite-id>) */
  outputDir?: string;

  /** Save optimized values to device on completion */
  saveToDevice?: boolean;

  /** Analyze boundaries and warn if optimal values are at limits */
  analyzeBoundaries?: boolean;
}

/**
 * Suite execution status
 */
export interface SuiteStatus {
  suiteId: string;
  startedAt: string;
  phase: 'baseline' | 'sweep' | 'validation' | 'complete' | 'failed';
  currentSweep?: string;
  progress: {
    baselineComplete: boolean;
    sweepsCompleted: string[];
    validationComplete: boolean;
  };
  error?: string;
}

// =============================================================================
// PREDEFINED SUITES
// =============================================================================

/**
 * Quick transient detection suite - fast optimization of key parameters
 */
export const SUITE_QUICK_TRANSIENT: SuiteConfig = {
  id: 'quick-transient',
  name: 'Quick Transient Optimization',
  description: 'Fast optimization of primary transient detection parameters',
  baseline: {
    modes: ['drummer', 'spectral', 'hybrid'],
    repetitions: 2,
  },
  sweeps: [
    { parameter: 'hitthresh' },
    { parameter: 'attackmult' },
    { parameter: 'cooldown' },
    { parameter: 'fluxthresh' },
  ],
  validation: {
    modes: ['drummer', 'spectral', 'hybrid'],
    useOptimized: true,
  },
  saveInterval: 'per-pattern',
  saveToDevice: true,
  analyzeBoundaries: true,
};

/**
 * Cooldown optimization suite - for fast-tempo patterns
 */
export const SUITE_COOLDOWN: SuiteConfig = {
  id: 'cooldown-optimization',
  name: 'Cooldown Optimization',
  description: 'Optimize cooldown for fast-tempo and simultaneous patterns',
  baseline: {
    modes: ['drummer', 'hybrid'],
    patterns: ['strong-beats', 'fast-tempo'],
    repetitions: 2,
  },
  sweeps: [
    {
      parameter: 'cooldown',
      values: [20, 30, 40, 50, 60, 80, 100],
      patterns: ['fast-tempo', 'simultaneous', 'cooldown-stress-40ms'],
    },
  ],
  validation: {
    modes: ['drummer', 'hybrid'],
    patterns: ['fast-tempo', 'simultaneous', 'strong-beats'],
    useOptimized: true,
  },
  saveInterval: 'per-pattern',
  saveToDevice: true,
  analyzeBoundaries: true,
};

/**
 * Threshold optimization suite - for sensitivity tuning
 */
export const SUITE_THRESHOLD: SuiteConfig = {
  id: 'threshold-optimization',
  name: 'Threshold Optimization',
  description: 'Optimize detection thresholds for sensitivity/false-positive balance',
  baseline: {
    modes: ['drummer', 'spectral', 'hybrid'],
    repetitions: 2,
  },
  sweeps: [
    {
      parameter: 'hitthresh',
      patterns: ['strong-beats', 'soft-beats', 'sparse'],
    },
    {
      parameter: 'fluxthresh',
      patterns: ['strong-beats', 'pad-rejection', 'chord-rejection'],
    },
    {
      parameter: 'attackmult',
      patterns: ['strong-beats', 'synth-stabs'],
    },
  ],
  validation: {
    modes: ['drummer', 'spectral', 'hybrid'],
    useOptimized: true,
  },
  saveInterval: 'per-pattern',
  saveToDevice: true,
  analyzeBoundaries: true,
};

/**
 * False positive reduction suite
 */
export const SUITE_FALSE_POSITIVE: SuiteConfig = {
  id: 'false-positive-reduction',
  name: 'False Positive Reduction',
  description: 'Reduce false positives on sustained sounds and sparse patterns',
  sweeps: [
    {
      parameter: 'fluxthresh',
      values: [1.4, 1.6, 1.8, 2.0, 2.2, 2.5],
      patterns: ['pad-rejection', 'chord-rejection', 'sparse'],
    },
    {
      parameter: 'hitthresh',
      patterns: ['pad-rejection', 'soft-beats'],
    },
  ],
  validation: {
    modes: ['spectral', 'hybrid'],
    patterns: ['pad-rejection', 'chord-rejection', 'sparse', 'strong-beats'],
    useOptimized: true,
  },
  saveInterval: 'per-pattern',
  analyzeBoundaries: true,
};

/**
 * Full calibration suite - comprehensive parameter optimization
 */
export const SUITE_FULL_CALIBRATION: SuiteConfig = {
  id: 'full-calibration',
  name: 'Full Calibration Suite',
  description: 'Comprehensive optimization of all transient detection parameters',
  baseline: {
    modes: ['drummer', 'spectral', 'hybrid'],
    repetitions: 3,
  },
  sweeps: [
    { parameter: 'hitthresh' },
    { parameter: 'attackmult' },
    { parameter: 'cooldown' },
    { parameter: 'fluxthresh' },
    { parameter: 'fluxbins' },
    { parameter: 'onsetthresh' },
    { parameter: 'risethresh' },
  ],
  validation: {
    modes: ['drummer', 'spectral', 'hybrid'],
    useOptimized: true,
  },
  saveInterval: 'per-pattern',
  saveToDevice: true,
  analyzeBoundaries: true,
};

/**
 * Registry of all predefined suites
 */
export const PREDEFINED_SUITES: Record<string, SuiteConfig> = {
  'quick-transient': SUITE_QUICK_TRANSIENT,
  'cooldown-optimization': SUITE_COOLDOWN,
  'threshold-optimization': SUITE_THRESHOLD,
  'false-positive-reduction': SUITE_FALSE_POSITIVE,
  'full-calibration': SUITE_FULL_CALIBRATION,
};

// =============================================================================
// SUITE RUNNER
// =============================================================================

/**
 * Runs a complete test suite with baseline, sweeps, and validation
 */
export class SuiteRunner {
  private config: SuiteConfig;
  private options: TunerOptions;
  private stateManager: StateManager;
  private status: SuiteStatus;

  constructor(config: SuiteConfig, options: TunerOptions) {
    this.config = config;
    this.options = {
      ...options,
      outputDir: config.outputDir || `tuning-output/${config.id}`,
    };
    this.stateManager = new StateManager(this.options.outputDir!);
    this.status = {
      suiteId: config.id,
      startedAt: new Date().toISOString(),
      phase: 'baseline',
      progress: {
        baselineComplete: false,
        sweepsCompleted: [],
        validationComplete: false,
      },
    };
  }

  /**
   * Run the complete suite
   */
  async run(): Promise<SuiteStatus> {
    console.log('\n' + '═'.repeat(60));
    console.log(`  TEST SUITE: ${this.config.name}`);
    console.log('═'.repeat(60));
    console.log(`  ${this.config.description}`);
    console.log(`  Save interval: ${this.config.saveInterval}`);
    if (this.config.saveToDevice) {
      console.log('  Will save optimized values to device on completion');
    }
    console.log('═'.repeat(60) + '\n');

    try {
      // Phase 1: Baseline (if configured)
      if (this.config.baseline) {
        await this.runBaselinePhase();
      }

      // Phase 2: Parameter Sweeps
      if (this.config.sweeps && this.config.sweeps.length > 0) {
        await this.runSweepPhase();
      }

      // Phase 3: Validation (if configured)
      if (this.config.validation) {
        await this.runValidationPhase();
      }

      // Post-sweep: Boundary analysis (if configured)
      if (this.config.analyzeBoundaries) {
        const boundaryReport = analyzeBoundaries(this.stateManager);
        printBoundaryReport(boundaryReport);
      }

      // Post-sweep: Save to device (if configured)
      if (this.config.saveToDevice) {
        const saveReport = await saveOptimizedToDevice(this.options, this.stateManager);
        if (!saveReport.success) {
          console.warn('\nWarning: Device save had issues. Check report above.');
        }
      }

      this.status.phase = 'complete';
      console.log('\n' + '═'.repeat(60));
      console.log('  SUITE COMPLETE');
      console.log('═'.repeat(60) + '\n');

      return this.status;

    } catch (error) {
      this.status.phase = 'failed';
      this.status.error = error instanceof Error ? error.message : String(error);
      console.error(`\nSuite failed: ${this.status.error}`);
      return this.status;
    }
  }

  /**
   * Run baseline phase
   */
  private async runBaselinePhase(): Promise<void> {
    this.status.phase = 'baseline';
    const config = this.config.baseline!;

    // Build options for baseline
    const baselineOptions: TunerOptions = {
      ...this.options,
      modes: config.modes,
      patterns: config.patterns,
    };

    await runBaseline(baselineOptions, this.stateManager);
    this.status.progress.baselineComplete = true;
  }

  /**
   * Run sweep phase
   */
  private async runSweepPhase(): Promise<void> {
    this.status.phase = 'sweep';

    for (const sweepConfig of this.config.sweeps!) {
      this.status.currentSweep = sweepConfig.parameter;

      // Get parameter definition
      const paramDef = PARAMETERS[sweepConfig.parameter];
      if (!paramDef) {
        console.warn(`Unknown parameter: ${sweepConfig.parameter}, skipping`);
        continue;
      }

      // Auto-select patterns if not specified
      let patterns = sweepConfig.patterns;
      if (!patterns || patterns.length === 0) {
        const autoPatterns = getPatternsForParam(sweepConfig.parameter);
        if (autoPatterns.length > 0) {
          patterns = autoPatterns.map(p => p.id);
          console.log(`Auto-selected patterns for ${sweepConfig.parameter}: ${patterns.join(', ')}`);
        }
      }

      // Build sweep options
      const sweepOptions: TunerOptions = {
        ...this.options,
        params: [sweepConfig.parameter],
        patterns: patterns,
        modes: sweepConfig.modes || (
          DETECTION_MODES.includes(paramDef.mode as DetectionMode)
            ? [paramDef.mode as DetectionMode]
            : undefined
        ),
      };

      // Custom sweep values are not yet supported
      if (sweepConfig.values) {
        throw new Error(
          `Custom sweep values are not yet supported for parameter '${sweepConfig.parameter}'. ` +
          `Remove 'values' from sweep config or modify PARAMETERS[${sweepConfig.parameter}].sweepValues instead.`
        );
      }

      await runSweeps(sweepOptions, this.stateManager);
      this.status.progress.sweepsCompleted.push(sweepConfig.parameter);
    }

    delete this.status.currentSweep;
  }

  /**
   * Run validation phase
   */
  private async runValidationPhase(): Promise<void> {
    this.status.phase = 'validation';
    const config = this.config.validation!;

    // Apply optimized parameters if requested
    if (config.useOptimized !== false) {
      console.log('\nApplying optimized parameters for validation...');
      // Optimal params are already stored in state, validation will use them
    }

    const validationOptions: TunerOptions = {
      ...this.options,
      modes: config.modes,
      patterns: config.patterns,
    };

    await runValidation(validationOptions, this.stateManager);
    this.status.progress.validationComplete = true;
  }

  /**
   * Get current suite status
   */
  getStatus(): SuiteStatus {
    return { ...this.status };
  }

  /**
   * Get state manager for external access
   */
  getStateManager(): StateManager {
    return this.stateManager;
  }
}

// =============================================================================
// SUITE UTILITIES
// =============================================================================

/**
 * List all available predefined suites
 */
export function listSuites(): void {
  console.log('\nAvailable Test Suites:');
  console.log('═'.repeat(50));

  for (const [id, suite] of Object.entries(PREDEFINED_SUITES)) {
    console.log(`\n  ${id}`);
    console.log(`    ${suite.name}`);
    console.log(`    ${suite.description}`);
    if (suite.sweeps) {
      const params = suite.sweeps.map(s => s.parameter).join(', ');
      console.log(`    Parameters: ${params}`);
    }
  }
  console.log();
}

/**
 * Get a suite by ID (predefined or custom)
 */
export function getSuite(id: string): SuiteConfig | undefined {
  return PREDEFINED_SUITES[id];
}

/**
 * Validate a suite configuration
 */
export function validateSuiteConfig(config: SuiteConfig): string[] {
  const errors: string[] = [];

  if (!config.id || !/^[a-z0-9-]+$/.test(config.id)) {
    errors.push('Suite ID must be kebab-case (lowercase with hyphens)');
  }

  if (!config.name) {
    errors.push('Suite name is required');
  }

  if (config.sweeps) {
    for (const sweep of config.sweeps) {
      if (!PARAMETERS[sweep.parameter]) {
        errors.push(`Unknown parameter: ${sweep.parameter}`);
      }
      if (sweep.patterns) {
        for (const pattern of sweep.patterns) {
          if (!PATTERN_REGISTRY[pattern]) {
            errors.push(`Unknown pattern: ${pattern}`);
          }
        }
      }
    }
  }

  if (config.baseline?.patterns) {
    for (const pattern of config.baseline.patterns) {
      if (!PATTERN_REGISTRY[pattern]) {
        errors.push(`Unknown baseline pattern: ${pattern}`);
      }
    }
  }

  if (config.validation?.patterns) {
    for (const pattern of config.validation.patterns) {
      if (!PATTERN_REGISTRY[pattern]) {
        errors.push(`Unknown validation pattern: ${pattern}`);
      }
    }
  }

  return errors;
}

/**
 * Create a suite from command-line options
 */
export function createSuiteFromOptions(options: TunerOptions): SuiteConfig {
  const sweeps: SweepConfig[] = [];

  // Filter to only detection modes (exclude 'music', 'rhythm' subsystem modes)
  const detectionModes: DetectionMode[] | undefined = options.modes?.filter(
    (m): m is DetectionMode => DETECTION_MODES.includes(m as DetectionMode)
  );

  // Add sweeps for each specified parameter
  if (options.params) {
    for (const param of options.params) {
      sweeps.push({
        parameter: param,
        patterns: options.patterns,
        modes: detectionModes,
      });
    }
  }

  return {
    id: `custom-${Date.now()}`,
    name: 'Custom Suite',
    description: 'Suite created from command-line options',
    baseline: {
      modes: detectionModes,
      patterns: options.patterns,
    },
    sweeps: sweeps.length > 0 ? sweeps : undefined,
    validation: {
      modes: detectionModes,
      patterns: options.patterns,
      useOptimized: true,
    },
    saveInterval: 'per-pattern',
    saveToDevice: false,
    analyzeBoundaries: true,
  };
}
