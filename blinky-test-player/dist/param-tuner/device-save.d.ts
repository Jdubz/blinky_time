/**
 * Device Configuration Persistence
 * Saves optimal parameters to device flash
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * Saves ensemble detector configuration.
 * Legacy per-mode saving has been removed.
 */
import type { TunerOptions } from './types.js';
import { StateManager } from './state.js';
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
export declare function saveOptimalToDevice(options: TunerOptions, stateManager: StateManager): Promise<DeviceSaveReport>;
/**
 * Show what would be saved without actually saving
 */
export declare function showOptimalParams(stateManager: StateManager): Promise<void>;
/**
 * Reset device to default parameters
 */
export declare function resetDeviceToDefaults(options: TunerOptions): Promise<void>;
/**
 * Print device save report
 */
export declare function printDeviceSaveReport(report: DeviceSaveReport): void;
