/**
 * Dynamic Parameter Bounds Extension
 * Tests edge cases to determine if parameter bounds should be extended
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * Tests ensemble parameter limits.
 * Legacy mode-specific testing has been removed.
 */
import type { TunerOptions } from './types.js';
interface BoundsTestResult {
    param: string;
    direction: 'lower' | 'upper';
    testedValue: number;
    f1: number;
    recommendExtend: boolean;
}
/**
 * Test parameter bounds to see if we should extend them
 */
export declare function testParameterBounds(options: TunerOptions, params?: string[]): Promise<BoundsTestResult[]>;
/**
 * Run extended bounds test for specific parameters
 */
export declare function runExtendedBoundsTest(options: TunerOptions): Promise<void>;
export {};
