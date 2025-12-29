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
import { StateManager } from './state.js';
import { runBaseline, showBaselineSummary } from './baseline.js';
import { runSweeps, showSweepSummary } from './sweep.js';
import { runInteractions, showInteractionSummary } from './interact.js';
import { runValidation, showValidationSummary } from './validate.js';
import { generateReport, showReportSummary } from './report.js';
import { runFastTune } from './fast-tune.js';

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

main().catch((err) => {
  console.error('Fatal error:', err);
  process.exit(1);
});
