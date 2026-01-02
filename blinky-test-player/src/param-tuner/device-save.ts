/**
 * Device Configuration Persistence
 * Saves optimal parameters to device flash
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * Saves ensemble detector configuration.
 * Legacy per-mode saving has been removed.
 */

import type { TunerOptions } from './types.js';
import { PARAMETERS } from './types.js';
import { StateManager } from './state.js';
import { TestRunner } from './runner.js';

/**
 * Result of a parameter save operation
 */
export interface ParameterSaveResult {
  parameter: string;
  oldValue: number;
  newValue: number;
  success: boolean;
  error?: string;
}

/**
 * Complete device save report
 */
export interface DeviceSaveReport {
  timestamp: string;
  success: boolean;
  parametersUpdated: ParameterSaveResult[];
  parametersFailed: ParameterSaveResult[];
  flashSaved: boolean;
  flashError?: string;
  summary: string;
}

/**
 * Save optimal parameters to device flash
 */
export async function saveOptimalToDevice(
  options: TunerOptions,
  stateManager: StateManager
): Promise<DeviceSaveReport> {
  console.log('\n Saving Optimal Parameters to Device');
  console.log('='.repeat(50));

  const report: DeviceSaveReport = {
    timestamp: new Date().toISOString(),
    success: true,
    parametersUpdated: [],
    parametersFailed: [],
    flashSaved: false,
    summary: '',
  };

  const runner = new TestRunner(options);
  await runner.connect();

  try {
    const optimalParams = stateManager.getOptimalParams();

    if (!optimalParams || Object.keys(optimalParams).length === 0) {
      report.summary = 'No optimal parameters found to save.';
      console.log(report.summary);
      return report;
    }

    console.log('\nApplying optimal ensemble parameters...');

    // Apply each parameter
    for (const [param, value] of Object.entries(optimalParams)) {
      try {
        await runner.setParameter(param, value);
        report.parametersUpdated.push({
          parameter: param,
          oldValue: PARAMETERS[param]?.default ?? 0,
          newValue: value,
          success: true,
        });
        console.log(`  set ${param} = ${value}`);
      } catch (err) {
        report.parametersFailed.push({
          parameter: param,
          oldValue: PARAMETERS[param]?.default ?? 0,
          newValue: value,
          success: false,
          error: String(err),
        });
        console.error(`  FAILED: ${param} = ${value}: ${err}`);
      }
    }

    // Save to flash
    console.log('\nSaving to flash...');
    try {
      await runner.saveToFlash();
      report.flashSaved = true;
      console.log('Flash save complete.');
    } catch (err) {
      report.flashError = String(err);
      report.success = false;
      console.error(`Flash save failed: ${err}`);
    }

    report.summary = `${report.parametersUpdated.length} parameters saved, ${report.parametersFailed.length} failed`;
    if (report.flashSaved) {
      report.summary += ', flash saved';
    }

    console.log(`\n${report.summary}`);

  } finally {
    await runner.disconnect();
  }

  return report;
}

/**
 * Show what would be saved without actually saving
 */
export async function showOptimalParams(
  stateManager: StateManager
): Promise<void> {
  console.log('\n Optimal Parameters');
  console.log('='.repeat(50));

  const optimalParams = stateManager.getOptimalParams();

  if (!optimalParams || Object.keys(optimalParams).length === 0) {
    console.log('No optimal parameters found.');
    return;
  }

  console.log('\nENSEMBLE:');
  for (const [param, value] of Object.entries(optimalParams)) {
    const def = PARAMETERS[param]?.default;
    const change = value !== def ? ` (default: ${def})` : '';
    console.log(`  ${param}: ${value}${change}`);
  }

  console.log('\nSerial commands to apply manually:');
  for (const [param, value] of Object.entries(optimalParams)) {
    const paramDef = PARAMETERS[param];
    if (paramDef?.command) {
      console.log(`  set ${paramDef.command} ${value}`);
    } else {
      console.log(`  set ${param} ${value}`);
    }
  }
  console.log('  save');
}

/**
 * Reset device to default parameters
 */
export async function resetDeviceToDefaults(
  options: TunerOptions
): Promise<void> {
  console.log('\n Resetting Device to Defaults');
  console.log('='.repeat(50));

  const runner = new TestRunner(options);
  await runner.connect();

  try {
    await runner.resetDefaults();
    await runner.saveToFlash();
    console.log('Device reset to default parameters and saved.');
  } finally {
    await runner.disconnect();
  }
}

/**
 * Print device save report
 */
export function printDeviceSaveReport(report: DeviceSaveReport): void {
  console.log('\n Device Save Report');
  console.log('='.repeat(50));
  console.log(`Timestamp: ${report.timestamp}`);
  console.log(`Success: ${report.success}`);
  console.log(`Parameters Updated: ${report.parametersUpdated.length}`);
  console.log(`Parameters Failed: ${report.parametersFailed.length}`);
  console.log(`Flash Saved: ${report.flashSaved}`);
  if (report.flashError) {
    console.log(`Flash Error: ${report.flashError}`);
  }
  console.log(`Summary: ${report.summary}`);
}
