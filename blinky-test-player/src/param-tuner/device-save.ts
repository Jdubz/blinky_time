/**
 * Device Save Module
 *
 * Saves optimized parameter values to the blinky device via serial.
 * Uses the blinky-serial MCP server for communication.
 */

import type { DetectionMode } from './types.js';
import { DETECTION_MODES, PARAMETERS } from './types.js';
import { StateManager } from './state.js';
import { TestRunner } from './runner.js';
import type { TunerOptions } from './types.js';

// =============================================================================
// TYPES
// =============================================================================

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

// =============================================================================
// DEVICE SAVE FUNCTIONS
// =============================================================================

/**
 * Save all optimized parameters to the device
 */
export async function saveOptimizedToDevice(
  options: TunerOptions,
  stateManager: StateManager,
  modes?: DetectionMode[]
): Promise<DeviceSaveReport> {
  const runner = new TestRunner(options);
  const report: DeviceSaveReport = {
    timestamp: new Date().toISOString(),
    success: false,
    parametersUpdated: [],
    parametersFailed: [],
    flashSaved: false,
    summary: '',
  };

  console.log('\n' + '='.repeat(60));
  console.log('  SAVING OPTIMIZED VALUES TO DEVICE');
  console.log('='.repeat(60));

  try {
    await runner.connect();

    // Get all sweep results
    const modesToSave = modes || [...DETECTION_MODES];

    for (const mode of modesToSave) {
      const optimalParams = stateManager.getOptimalParams(mode);
      if (!optimalParams) {
        console.log(`\n  No optimal params found for ${mode}, skipping`);
        continue;
      }

      console.log(`\n  Setting ${mode} parameters...`);

      // First set the mode
      await runner.setMode(mode);

      // Then set each parameter
      for (const [paramName, newValue] of Object.entries(optimalParams)) {
        const paramDef = PARAMETERS[paramName];
        if (!paramDef) continue;

        const oldValue = paramDef.default;

        try {
          await runner.setParameter(paramName, newValue);
          report.parametersUpdated.push({
            parameter: paramName,
            oldValue,
            newValue,
            success: true,
          });
          console.log(`    ${paramName}: ${oldValue} -> ${newValue}`);
        } catch (error) {
          report.parametersFailed.push({
            parameter: paramName,
            oldValue,
            newValue,
            success: false,
            error: error instanceof Error ? error.message : String(error),
          });
          console.error(`    ${paramName}: FAILED - ${error}`);
        }
      }
    }

    // Save to flash
    console.log('\n  Saving to device flash memory...');
    try {
      await runner.saveToFlash();
      report.flashSaved = true;
      console.log('    Flash save: SUCCESS');
    } catch (error) {
      report.flashError = error instanceof Error ? error.message : String(error);
      console.error(`    Flash save: FAILED - ${report.flashError}`);
    }

    report.success = report.parametersFailed.length === 0 && report.flashSaved;

    if (report.success) {
      report.summary = `Successfully saved ${report.parametersUpdated.length} parameters to device flash.`;
    } else {
      const issues: string[] = [];
      if (report.parametersFailed.length > 0) {
        issues.push(`${report.parametersFailed.length} parameters failed`);
      }
      if (!report.flashSaved) {
        issues.push('flash save failed');
      }
      report.summary = `Partial save: ${issues.join(', ')}`;
    }

  } catch (error) {
    report.summary = `Device connection failed: ${error instanceof Error ? error.message : String(error)}`;
    console.error(`\n  Error: ${report.summary}`);
  } finally {
    await runner.disconnect();
  }

  console.log('\n  ' + report.summary);
  console.log('='.repeat(60) + '\n');

  return report;
}

/**
 * Get a summary of changes that would be saved
 */
export function previewDeviceSave(
  stateManager: StateManager,
  modes?: DetectionMode[]
): void {
  console.log('\n' + '='.repeat(60));
  console.log('  DEVICE SAVE PREVIEW (dry-run)');
  console.log('='.repeat(60));

  const modesToShow = modes || [...DETECTION_MODES];
  let totalChanges = 0;

  for (const mode of modesToShow) {
    const optimalParams = stateManager.getOptimalParams(mode);
    if (!optimalParams) {
      console.log(`\n  ${mode}: No optimal params found`);
      continue;
    }

    console.log(`\n  ${mode.toUpperCase()}:`);

    for (const [paramName, newValue] of Object.entries(optimalParams)) {
      const paramDef = PARAMETERS[paramName];
      if (!paramDef) continue;

      const oldValue = paramDef.default;
      const changed = oldValue !== newValue;

      if (changed) {
        console.log(`    ${paramName}: ${oldValue} -> ${newValue} (CHANGE)`);
        totalChanges++;
      } else {
        console.log(`    ${paramName}: ${newValue} (unchanged)`);
      }
    }
  }

  console.log('\n  Summary:');
  console.log(`    Total parameters to update: ${totalChanges}`);
  console.log('='.repeat(60) + '\n');
}

/**
 * Print device save report
 */
export function printDeviceSaveReport(report: DeviceSaveReport): void {
  console.log('\n' + '='.repeat(60));
  console.log('  DEVICE SAVE REPORT');
  console.log('='.repeat(60));

  console.log(`\n  Status: ${report.success ? 'SUCCESS' : 'FAILED'}`);
  console.log(`  Time: ${report.timestamp}`);

  if (report.parametersUpdated.length > 0) {
    console.log('\n  Updated Parameters:');
    for (const p of report.parametersUpdated) {
      console.log(`    ${p.parameter}: ${p.oldValue} -> ${p.newValue}`);
    }
  }

  if (report.parametersFailed.length > 0) {
    console.log('\n  Failed Parameters:');
    for (const p of report.parametersFailed) {
      console.log(`    ${p.parameter}: ${p.error}`);
    }
  }

  console.log('\n  Flash Save:', report.flashSaved ? 'SUCCESS' : `FAILED - ${report.flashError}`);
  console.log('\n  Summary:', report.summary);
  console.log('='.repeat(60) + '\n');
}
