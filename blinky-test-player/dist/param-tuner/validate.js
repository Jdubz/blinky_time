/**
 * Phase 4: Validation
 * Validates optimal parameters across ALL patterns
 */
import cliProgress from 'cli-progress';
import { DETECTION_MODES, ALL_PATTERNS, PARAMETERS } from './types.js';
import { TestRunner } from './runner.js';
export async function runValidation(options, stateManager) {
    console.log('\n✓ Phase 4: Validation');
    console.log('═'.repeat(50));
    console.log('Validating optimal parameters across all patterns.\n');
    const runner = new TestRunner(options);
    await runner.connect();
    try {
        const patterns = ALL_PATTERNS;
        for (const mode of DETECTION_MODES) {
            if (stateManager.isValidationComplete(mode)) {
                const existing = stateManager.getValidationResult(mode);
                if (existing) {
                    console.log(`✓ ${mode}: Already validated (F1: ${existing.overall.f1}, Δ: ${existing.vsBaseline.f1Delta > 0 ? '+' : ''}${existing.vsBaseline.f1Delta})`);
                }
                continue;
            }
            console.log(`\nValidating ${mode} algorithm...`);
            stateManager.setValidationInProgress(mode);
            // Get optimal parameters
            const optimalParams = stateManager.getOptimalParams(mode);
            if (!optimalParams) {
                console.log(`   No optimal parameters found, using defaults`);
                continue;
            }
            console.log(`   Using optimal params: ${JSON.stringify(optimalParams)}`);
            // Set mode and apply optimal parameters
            await runner.setMode(mode);
            for (const [param, value] of Object.entries(optimalParams)) {
                await runner.setParameter(param, value);
            }
            // Progress bar
            const progress = new cliProgress.SingleBar({
                format: '   {bar} {percentage}% | {value}/{total} patterns | {eta_formatted}',
                barCompleteChar: '█',
                barIncompleteChar: '░',
                hideCursor: true,
            });
            const results = {};
            let totalF1 = 0;
            let totalPrecision = 0;
            let totalRecall = 0;
            progress.start(patterns.length, 0);
            for (const pattern of patterns) {
                try {
                    // Run pattern 3 times for consistency
                    let sumF1 = 0;
                    let sumPrecision = 0;
                    let sumRecall = 0;
                    let lastResult = null;
                    for (let run = 0; run < 3; run++) {
                        const result = await runner.runPattern(pattern);
                        sumF1 += result.f1;
                        sumPrecision += result.precision;
                        sumRecall += result.recall;
                        lastResult = result;
                    }
                    if (lastResult) {
                        results[pattern] = {
                            ...lastResult,
                            f1: Math.round((sumF1 / 3) * 1000) / 1000,
                            precision: Math.round((sumPrecision / 3) * 1000) / 1000,
                            recall: Math.round((sumRecall / 3) * 1000) / 1000,
                        };
                        totalF1 += results[pattern].f1;
                        totalPrecision += results[pattern].precision;
                        totalRecall += results[pattern].recall;
                    }
                }
                catch (err) {
                    console.error(`\n   Error on ${pattern}:`, err);
                }
                progress.increment();
            }
            progress.stop();
            const n = Object.keys(results).length;
            if (n === 0) {
                console.error(`   No patterns completed for ${mode}, skipping validation`);
                continue;
            }
            const avgF1 = Math.round((totalF1 / n) * 1000) / 1000;
            const avgPrecision = Math.round((totalPrecision / n) * 1000) / 1000;
            const avgRecall = Math.round((totalRecall / n) * 1000) / 1000;
            // Compare to baseline
            const baseline = stateManager.getBaselineResult(mode);
            const baselineF1 = baseline?.overall.avgF1 ?? 0;
            const f1Delta = Math.round((avgF1 - baselineF1) * 1000) / 1000;
            // Find improved and regressed patterns
            const improved = [];
            const regressed = [];
            if (baseline) {
                for (const pattern of Object.keys(results)) {
                    const validResult = results[pattern];
                    const baseResult = baseline.patterns[pattern];
                    if (baseResult) {
                        const diff = validResult.f1 - baseResult.f1;
                        if (diff > 0.05) {
                            improved.push(pattern);
                        }
                        else if (diff < -0.05) {
                            regressed.push(pattern);
                        }
                    }
                }
            }
            const validation = {
                algorithm: mode,
                params: optimalParams,
                timestamp: new Date().toISOString(),
                patterns: results,
                overall: {
                    f1: avgF1,
                    precision: avgPrecision,
                    recall: avgRecall,
                },
                vsBaseline: {
                    f1Delta,
                    improved,
                    regressed,
                },
            };
            stateManager.saveValidationResult(mode, validation);
            console.log(`   F1: ${avgF1} | Precision: ${avgPrecision} | Recall: ${avgRecall}`);
            console.log(`   vs Baseline: ${f1Delta > 0 ? '+' : ''}${f1Delta}`);
            if (improved.length > 0) {
                console.log(`   Improved: ${improved.join(', ')}`);
            }
            if (regressed.length > 0) {
                console.log(`   ⚠️ Regressed: ${regressed.join(', ')}`);
            }
        }
        stateManager.markValidationPhaseComplete();
        console.log('\n✅ Validation phase complete.\n');
    }
    finally {
        await runner.disconnect();
    }
}
export async function showValidationSummary(stateManager) {
    console.log('\n✓ Validation Summary');
    console.log('═'.repeat(50));
    let bestMode = null;
    let bestF1 = 0;
    for (const mode of DETECTION_MODES) {
        const result = stateManager.getValidationResult(mode);
        const baseline = stateManager.getBaselineResult(mode);
        console.log(`\n${mode.toUpperCase()}`);
        if (!result) {
            console.log('  Not yet validated');
            continue;
        }
        console.log(`  F1: ${result.overall.f1} | Precision: ${result.overall.precision} | Recall: ${result.overall.recall}`);
        if (baseline) {
            const delta = result.vsBaseline.f1Delta;
            console.log(`  vs Baseline: ${delta > 0 ? '+' : ''}${delta}`);
        }
        if (result.overall.f1 > bestF1) {
            bestF1 = result.overall.f1;
            bestMode = mode;
        }
        console.log('  Optimal params:');
        for (const [param, value] of Object.entries(result.params)) {
            const def = PARAMETERS[param]?.default;
            const change = value !== def ? ` (default: ${def})` : '';
            console.log(`    ${param}: ${value}${change}`);
        }
    }
    if (bestMode) {
        console.log(`\n🏆 Best algorithm: ${bestMode.toUpperCase()} (F1: ${bestF1})`);
    }
}
