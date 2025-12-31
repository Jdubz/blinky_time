#!/usr/bin/env node
/**
 * Parameter Tuner CLI
 * Systematically tests detection parameters to find optimal settings
 */

import yargs from 'yargs';
import { hideBin } from 'yargs/helpers';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import type { TunerOptions } from './types.js';
import { PARAMETERS } from './types.js';
import { StateManager } from './state.js';
import { runBaseline, showBaselineSummary } from './baseline.js';
import { runSweeps, showSweepSummary } from './sweep.js';
import { runInteractions, showInteractionSummary } from './interact.js';
import { runValidation, showValidationSummary } from './validate.js';
import { generateReport, showReportSummary } from './report.js';
import { runFastTune } from './fast-tune.js';
import { SuiteRunner, listSuites, getSuite, PREDEFINED_SUITES, validateSuiteConfig } from './suite.js';
import { QueueManager, createQueue, listQueues } from './queue.js';
import { getPatternsForParam } from '../patterns.js';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

const DEFAULT_OUTPUT_DIR = join(__dirname, '..', '..', 'tuning-results');

interface GlobalArgs {
  port?: string;
  gain?: number;
  output?: string;
  params?: string;
  modes?: string;
  patterns?: string;
}

async function main() {
  await yargs(hideBin(process.argv))
    .scriptName('param-tuner')
    .usage('$0 <command> [options]')
    .option('port', {
      alias: 'p',
      type: 'string',
      description: 'Serial port (e.g., COM5)',
    })
    .option('gain', {
      alias: 'g',
      type: 'number',
      description: 'Hardware gain to lock during tests (0-80)',
    })
    .option('output', {
      alias: 'o',
      type: 'string',
      default: DEFAULT_OUTPUT_DIR,
      description: 'Output directory for results',
    })
    .option('params', {
      type: 'string',
      description: 'Comma-separated param names to tune (default: all)',
      example: '--params hitthresh,attackmult,musicthresh',
    })
    .option('modes', {
      type: 'string',
      description: 'Comma-separated modes to tune (drummer,spectral,hybrid,bass,hfc,music,rhythm)',
      example: '--modes music,rhythm',
    })
    .option('patterns', {
      type: 'string',
      description: 'Comma-separated test patterns to use (default: all representative patterns)',
      example: '--patterns strong-beats,simple-4-on-floor',
    })
    .command('fast', 'Fast tuning with binary search (~30 min)', {}, async (args) => {
      await runFast(args as GlobalArgs);
    })
    .command('full', 'Run complete tuning suite (all phases, 4-6 hrs)', {}, async (args) => {
      await runFull(args as GlobalArgs);
    })
    .command('baseline', 'Run Phase 1: Baseline testing', {}, async (args) => {
      await runPhase('baseline', args as GlobalArgs);
    })
    .command('sweep', 'Run Phase 2: Parameter sweeps', {}, async (args) => {
      await runPhase('sweep', args as GlobalArgs);
    })
    .command('interact', 'Run Phase 3: Interaction tests', {}, async (args) => {
      await runPhase('interact', args as GlobalArgs);
    })
    .command('validate', 'Run Phase 4: Validation', {}, async (args) => {
      await runPhase('validate', args as GlobalArgs);
    })
    .command('report', 'Generate reports from existing results', {}, async (args) => {
      await runPhase('report', args as GlobalArgs);
    })
    .command('resume', 'Resume from last checkpoint', {}, async (args) => {
      await runResume(args as GlobalArgs);
    })
    .command('status', 'Show current tuning status', {}, async (args) => {
      await showStatus(args as GlobalArgs);
    })
    .command('reset', 'Reset all tuning state', {}, async (args) => {
      await resetState(args as GlobalArgs);
    })
    // NEW: Suite commands
    .command('suite <name>', 'Run a predefined test suite', (yargs) => {
      return yargs
        .positional('name', {
          describe: 'Suite name (use "suites" command to list available)',
          type: 'string',
        })
        .option('save-to-device', {
          type: 'boolean',
          default: false,
          description: 'Save optimized values to device flash on completion',
        });
    }, async (args) => {
      await runSuiteCommand(args as GlobalArgs & { name: string; 'save-to-device'?: boolean });
    })
    .command('suites', 'List available test suites', {}, () => {
      listSuites();
    })
    // NEW: Queue commands
    .command('queue', 'Manage and run test queues', (yargs) => {
      return yargs
        .option('suites', {
          type: 'string',
          description: 'Comma-separated suite IDs to queue',
        })
        .option('id', {
          type: 'string',
          description: 'Queue ID for resume/status',
        })
        .option('status', {
          type: 'boolean',
          description: 'Show queue status instead of running',
        })
        .option('list', {
          type: 'boolean',
          description: 'List all saved queues',
        })
        .option('clear', {
          type: 'boolean',
          description: 'Clear the specified queue',
        });
    }, async (args) => {
      await runQueueCommand(args as GlobalArgs & {
        suites?: string;
        id?: string;
        status?: boolean;
        list?: boolean;
        clear?: boolean;
      });
    })
    // NEW: Parameter-targeted quick test
    .command('target <param>', 'Quick test targeting a specific parameter', (yargs) => {
      return yargs
        .positional('param', {
          describe: 'Parameter name to optimize',
          type: 'string',
        })
        .option('save-to-device', {
          type: 'boolean',
          default: false,
          description: 'Save optimized value to device flash',
        });
    }, async (args) => {
      await runTargetCommand(args as GlobalArgs & { param: string; 'save-to-device'?: boolean });
    })
    .demandCommand(1, 'You must provide a command')
    .help()
    .alias('h', 'help')
    .parse();
}

function validatePort(args: GlobalArgs): string {
  if (!args.port) {
    console.error('Error: --port is required for this command');
    process.exit(1);
  }
  return args.port;
}

function createOptions(args: GlobalArgs, requirePort = true): TunerOptions {
  return {
    port: requirePort ? validatePort(args) : args.port || '',
    gain: args.gain,
    outputDir: args.output || DEFAULT_OUTPUT_DIR,
    params: args.params ? args.params.split(',').map(p => p.trim()) : undefined,
    modes: args.modes ? args.modes.split(',').map(m => m.trim() as any) : undefined,
    patterns: args.patterns ? args.patterns.split(',').map(p => p.trim()) : undefined,
  };
}

async function runFast(args: GlobalArgs): Promise<void> {
  const options = createOptions(args);

  try {
    await runFastTune(options);
  } catch (err) {
    console.error('\n Error:', err);
    process.exit(1);
  }
}

async function runFull(args: GlobalArgs): Promise<void> {
  console.log('\n Blinky Parameter Tuner v1.0');
  console.log('='.repeat(50));
  console.log('Running complete parameter tuning suite.\n');
  console.log('Estimated time: 4-6 hours');
  console.log('Press Ctrl+C to pause (progress will be saved).\n');

  const options = createOptions(args);
  const stateManager = new StateManager(options.outputDir!);

  try {
    // Phase 1: Baseline
    await runBaseline(options, stateManager);

    // Phase 2: Sweeps
    await runSweeps(options, stateManager);

    // Phase 3: Interactions
    await runInteractions(options, stateManager);

    // Phase 4: Validation
    await runValidation(options, stateManager);

    // Phase 5: Report
    await generateReport(options.outputDir!, stateManager);

    console.log('\n🎉 Parameter tuning complete!');
    showReportSummary(options.outputDir!);

  } catch (err) {
    console.error('\n❌ Error:', err);
    console.log('\nProgress saved. Run `param-tuner resume` to continue.');
    process.exit(1);
  }
}

async function runPhase(phase: string, args: GlobalArgs): Promise<void> {
  const needsPort = phase !== 'report';
  const options = createOptions(args, needsPort);
  const stateManager = new StateManager(options.outputDir!);

  try {
    switch (phase) {
      case 'baseline':
        await runBaseline(options, stateManager);
        break;
      case 'sweep':
        await runSweeps(options, stateManager);
        break;
      case 'interact':
        await runInteractions(options, stateManager);
        break;
      case 'validate':
        await runValidation(options, stateManager);
        break;
      case 'report':
        await generateReport(options.outputDir!, stateManager);
        showReportSummary(options.outputDir!);
        break;
    }
  } catch (err) {
    console.error('\n❌ Error:', err);
    console.log('\nProgress saved. Run `param-tuner resume` to continue.');
    process.exit(1);
  }
}

async function runResume(args: GlobalArgs): Promise<void> {
  const options = createOptions(args);
  const stateManager = new StateManager(options.outputDir!);
  const state = stateManager.getState();

  if (!stateManager.hasResumableState()) {
    console.log('\nNo previous run to resume. Starting fresh with `param-tuner full`.');
    return;
  }

  console.log('\n🔄 Resuming from previous run...');
  console.log(`   Last phase: ${state.currentPhase}`);
  console.log(`   Completed: ${state.phasesCompleted.join(', ') || 'none'}`);

  try {
    // Resume from current phase
    switch (state.currentPhase) {
      case 'baseline':
        await runBaseline(options, stateManager);
        await runSweeps(options, stateManager);
        await runInteractions(options, stateManager);
        await runValidation(options, stateManager);
        await generateReport(options.outputDir!, stateManager);
        break;
      case 'sweep':
        await runSweeps(options, stateManager);
        await runInteractions(options, stateManager);
        await runValidation(options, stateManager);
        await generateReport(options.outputDir!, stateManager);
        break;
      case 'interact':
        await runInteractions(options, stateManager);
        await runValidation(options, stateManager);
        await generateReport(options.outputDir!, stateManager);
        break;
      case 'validate':
        await runValidation(options, stateManager);
        await generateReport(options.outputDir!, stateManager);
        break;
      case 'report':
        await generateReport(options.outputDir!, stateManager);
        break;
      case 'done':
        console.log('\n✅ Tuning already complete!');
        showReportSummary(options.outputDir!);
        return;
    }

    console.log('\n🎉 Parameter tuning complete!');
    showReportSummary(options.outputDir!);

  } catch (err) {
    console.error('\n❌ Error:', err);
    console.log('\nProgress saved. Run `param-tuner resume` to continue.');
    process.exit(1);
  }
}

async function showStatus(args: GlobalArgs): Promise<void> {
  const outputDir = args.output || DEFAULT_OUTPUT_DIR;
  const stateManager = new StateManager(outputDir);
  const state = stateManager.getState();

  console.log('\n📊 Tuning Status');
  console.log('═'.repeat(50));
  console.log(`Output directory: ${outputDir}`);
  console.log(`Last updated: ${state.lastUpdated}`);
  console.log(`Current phase: ${state.currentPhase}`);
  console.log(`Completed phases: ${state.phasesCompleted.join(', ') || 'none'}`);

  // Show summaries
  if (state.phasesCompleted.includes('baseline')) {
    await showBaselineSummary(stateManager);
  }

  if (state.phasesCompleted.includes('sweep')) {
    await showSweepSummary(stateManager);
  }

  if (state.phasesCompleted.includes('interact')) {
    await showInteractionSummary(stateManager);
  }

  if (state.phasesCompleted.includes('validate')) {
    await showValidationSummary(stateManager);
  }

  if (state.phasesCompleted.includes('report')) {
    showReportSummary(outputDir);
  }
}

async function resetState(args: GlobalArgs): Promise<void> {
  const outputDir = args.output || DEFAULT_OUTPUT_DIR;
  const stateManager = new StateManager(outputDir);

  console.log('\n⚠️  Resetting tuning state...');
  stateManager.reset();
  console.log('✅ State reset. All progress cleared.\n');
}

// =============================================================================
// SUITE COMMANDS
// =============================================================================

async function runSuiteCommand(args: GlobalArgs & { name: string; 'save-to-device'?: boolean }): Promise<void> {
  const suiteConfig = getSuite(args.name);
  if (!suiteConfig) {
    console.error(`Unknown suite: ${args.name}`);
    console.log('\nAvailable suites:');
    for (const id of Object.keys(PREDEFINED_SUITES)) {
      console.log(`  - ${id}`);
    }
    process.exit(1);
  }

  // Create a copy to avoid mutating the predefined suite
  const suite = { ...suiteConfig };

  // Validate the suite
  const errors = validateSuiteConfig(suite);
  if (errors.length > 0) {
    console.error('Suite configuration errors:');
    for (const error of errors) {
      console.error(`  - ${error}`);
    }
    process.exit(1);
  }

  const options = createOptions(args);

  // Override save-to-device from CLI
  if (args['save-to-device']) {
    suite.saveToDevice = true;
  }

  try {
    const runner = new SuiteRunner(suite, options);
    const result = await runner.run();

    if (result.phase === 'failed') {
      console.error(`\nSuite failed: ${result.error}`);
      process.exit(1);
    }

    console.log('\nSuite completed successfully!');
  } catch (err) {
    console.error('\nSuite error:', err);
    process.exit(1);
  }
}

// =============================================================================
// QUEUE COMMANDS
// =============================================================================

async function runQueueCommand(args: GlobalArgs & {
  suites?: string;
  id?: string;
  status?: boolean;
  list?: boolean;
  clear?: boolean;
}): Promise<void> {
  const outputDir = args.output || DEFAULT_OUTPUT_DIR;

  // List all queues
  if (args.list) {
    const queues = listQueues(outputDir);
    if (queues.length === 0) {
      console.log('\nNo saved queues found.');
    } else {
      console.log('\nSaved queues:');
      for (const q of queues) {
        console.log(`  - ${q}`);
      }
    }
    return;
  }

  // Need queue ID for most operations
  const queueId = args.id || `queue-${Date.now()}`;
  // Second arg (requirePort=false) allows status/clear operations without --port
  const options = createOptions(args, false);

  // Show status
  if (args.status) {
    const manager = new QueueManager(queueId, options);
    manager.printStatus();
    return;
  }

  // Clear queue
  if (args.clear) {
    const manager = new QueueManager(queueId, options);
    manager.clear();
    return;
  }

  // Create and run queue from suite list
  if (args.suites) {
    if (!args.port) {
      console.error('Error: --port is required to create and run a queue');
      process.exit(1);
    }

    const suiteIds = args.suites.split(',').map(s => s.trim());

    // Validate all suites exist
    for (const id of suiteIds) {
      if (!getSuite(id)) {
        console.error(`Unknown suite: ${id}`);
        console.log('\nAvailable suites:');
        for (const sid of Object.keys(PREDEFINED_SUITES)) {
          console.log(`  - ${sid}`);
        }
        process.exit(1);
      }
    }

    const manager = createQueue(queueId, suiteIds, createOptions(args));
    console.log(`\nCreated queue "${queueId}" with ${suiteIds.length} suites`);

    try {
      await manager.run();
    } catch (err) {
      console.error('\nQueue error:', err);
      process.exit(1);
    }
    return;
  }

  // Resume existing queue
  if (args.id) {
    const manager = new QueueManager(args.id, createOptions(args));
    if (manager.getState().suites.length === 0) {
      console.error(`Queue "${args.id}" not found or empty`);
      process.exit(1);
    }

    try {
      await manager.run();
    } catch (err) {
      console.error('\nQueue error:', err);
      process.exit(1);
    }
    return;
  }

  // No action specified
  console.log('\nQueue command options:');
  console.log('  --suites <ids>  Create and run queue from comma-separated suite IDs');
  console.log('  --id <id>       Resume or check status of existing queue');
  console.log('  --status        Show queue status (requires --id)');
  console.log('  --list          List all saved queues');
  console.log('  --clear         Clear a queue (requires --id)');
}

// =============================================================================
// TARGET COMMAND
// =============================================================================

async function runTargetCommand(args: GlobalArgs & { param: string; 'save-to-device'?: boolean }): Promise<void> {
  const param = PARAMETERS[args.param];
  if (!param) {
    console.error(`Unknown parameter: ${args.param}`);
    console.log('\nAvailable parameters:');
    const paramNames = Object.keys(PARAMETERS).sort();
    for (const name of paramNames) {
      const p = PARAMETERS[name];
      console.log(`  - ${name} (${p.mode}): ${p.description}`);
    }
    process.exit(1);
  }

  // Get patterns that target this parameter
  const patterns = getPatternsForParam(args.param);
  const patternIds = patterns.map(p => p.id);

  if (patternIds.length === 0) {
    console.log(`\nNo patterns specifically target "${args.param}".`);
    console.log('Using default representative patterns for sweep.');
  } else {
    console.log(`\nPatterns targeting "${args.param}":`);
    for (const p of patterns) {
      console.log(`  - ${p.id}: ${p.name}`);
    }
  }

  const options = createOptions(args);

  // Create a minimal suite for this parameter
  const suite = {
    id: `target-${args.param}`,
    name: `Target: ${args.param}`,
    description: `Quick optimization of ${args.param}`,
    sweeps: [{
      parameter: args.param,
      patterns: patternIds.length > 0 ? patternIds : undefined,
    }],
    saveInterval: 'per-pattern' as const,
    saveToDevice: args['save-to-device'] || false,
    analyzeBoundaries: true,
  };

  try {
    const runner = new SuiteRunner(suite, options);
    const result = await runner.run();

    if (result.phase === 'failed') {
      console.error(`\nTarget test failed: ${result.error}`);
      process.exit(1);
    }

    // Show optimal value
    const stateManager = runner.getStateManager();
    const sweepResult = stateManager.getSweepResult(args.param);
    if (sweepResult) {
      console.log(`\nOptimal value for ${args.param}: ${sweepResult.optimal.value}`);
      console.log(`  F1 score: ${sweepResult.optimal.avgF1}`);
      console.log(`  Default was: ${param.default}`);

      if (sweepResult.optimal.value !== param.default) {
        console.log(`\n  Recommendation: Update ${args.param} from ${param.default} to ${sweepResult.optimal.value}`);
      }
    }

  } catch (err) {
    console.error('\nTarget test error:', err);
    process.exit(1);
  }
}

main().catch((err) => {
  console.error('Fatal error:', err);
  process.exit(1);
});
