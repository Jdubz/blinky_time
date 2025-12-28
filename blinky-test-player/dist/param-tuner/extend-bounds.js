/**
 * Extended bounds testing for parameters that hit their limits
 */
import { TestRunner } from './runner.js';
const TEST_PATTERNS = ['strong-beats', 'bass-line', 'synth-stabs', 'fast-tempo'];
export async function runExtendedBoundsTest(options) {
    console.log('\n Extended Bounds Testing');
    console.log('='.repeat(50));
    console.log('Testing parameters that hit their limits\n');
    const runner = new TestRunner(options);
    await runner.connect();
    try {
        // Set to hybrid mode (best performer)
        await runner.setMode('hybrid');
        // Start with the current optimal values
        await runner.setParameter('hitthresh', 2.063);
        await runner.setParameter('hyfluxwt', 0.3);
        await runner.setParameter('hydrumwt', 0.3);
        await runner.setParameter('cooldown', 40);
        const results = {};
        // Test 1: Extended hyfluxwt (lower bound)
        console.log('\n1. Testing hyfluxwt [0.1, 0.15, 0.2, 0.25, 0.3]');
        console.log('-'.repeat(40));
        results.hyfluxwt = await sweepParameter(runner, 'hyfluxwt', [0.1, 0.15, 0.2, 0.25, 0.3]);
        const bestFluxWt = findBest(results.hyfluxwt);
        console.log(`   Best: ${bestFluxWt.value} (F1: ${bestFluxWt.f1})`);
        await runner.setParameter('hyfluxwt', bestFluxWt.value);
        // Test 2: Extended hydrumwt (lower bound)
        console.log('\n2. Testing hydrumwt [0.1, 0.15, 0.2, 0.25, 0.3]');
        console.log('-'.repeat(40));
        results.hydrumwt = await sweepParameter(runner, 'hydrumwt', [0.1, 0.15, 0.2, 0.25, 0.3]);
        const bestDrumWt = findBest(results.hydrumwt);
        console.log(`   Best: ${bestDrumWt.value} (F1: ${bestDrumWt.f1})`);
        await runner.setParameter('hydrumwt', bestDrumWt.value);
        // Test 3: Extended cooldown (lower bound)
        console.log('\n3. Testing cooldown [20, 25, 30, 35, 40]');
        console.log('-'.repeat(40));
        results.cooldown = await sweepParameter(runner, 'cooldown', [20, 25, 30, 35, 40]);
        const bestCooldown = findBest(results.cooldown);
        console.log(`   Best: ${bestCooldown.value} (F1: ${bestCooldown.f1})`);
        await runner.setParameter('cooldown', bestCooldown.value);
        // Test 4: Extended fluxthresh (upper bound) - need spectral mode
        console.log('\n4. Testing fluxthresh [2.4, 2.6, 2.8, 3.0, 3.2]');
        console.log('-'.repeat(40));
        await runner.setMode('spectral');
        results.fluxthresh = await sweepParameter(runner, 'fluxthresh', [2.4, 2.6, 2.8, 3.0, 3.2]);
        const bestFluxThresh = findBest(results.fluxthresh);
        console.log(`   Best: ${bestFluxThresh.value} (F1: ${bestFluxThresh.f1})`);
        // Summary
        console.log('\n' + '='.repeat(50));
        console.log(' EXTENDED BOUNDS RESULTS');
        console.log('='.repeat(50));
        console.log('\nHybrid mode updates:');
        printComparison('hyfluxwt', 0.3, bestFluxWt);
        printComparison('hydrumwt', 0.3, bestDrumWt);
        printComparison('cooldown', 40, bestCooldown);
        console.log('\nSpectral mode updates:');
        printComparison('fluxthresh', 2.641, bestFluxThresh);
        // Save results
        const { writeFileSync } = await import('fs');
        const { join } = await import('path');
        const outputDir = options.outputDir || 'tuning-results';
        writeFileSync(join(outputDir, 'extended-bounds-results.json'), JSON.stringify({
            timestamp: new Date().toISOString(),
            results,
            optimal: {
                hyfluxwt: bestFluxWt.value,
                hydrumwt: bestDrumWt.value,
                cooldown: bestCooldown.value,
                fluxthresh: bestFluxThresh.value,
            }
        }, null, 2));
        console.log(`\nResults saved to ${join(outputDir, 'extended-bounds-results.json')}`);
    }
    finally {
        await runner.disconnect();
    }
}
async function sweepParameter(runner, param, values) {
    const points = [];
    for (const value of values) {
        await runner.setParameter(param, value);
        let totalF1 = 0;
        let totalP = 0;
        let totalR = 0;
        for (const pattern of TEST_PATTERNS) {
            const result = await runner.runPattern(pattern);
            totalF1 += result.f1;
            totalP += result.precision;
            totalR += result.recall;
        }
        const n = TEST_PATTERNS.length;
        const point = {
            value,
            f1: round(totalF1 / n),
            precision: round(totalP / n),
            recall: round(totalR / n),
        };
        points.push(point);
        console.log(`   ${param}=${value}: F1=${point.f1} P=${point.precision} R=${point.recall}`);
    }
    return points;
}
function findBest(points) {
    return points.reduce((a, b) => a.f1 > b.f1 ? a : b);
}
function printComparison(param, oldVal, best) {
    const change = best.value !== oldVal
        ? ` <- CHANGE from ${oldVal}`
        : ' (unchanged)';
    console.log(`  ${param}: ${best.value} (F1: ${best.f1})${change}`);
}
function round(n) {
    return Math.round(n * 1000) / 1000;
}
// CLI entry
const args = process.argv.slice(2);
const portIdx = args.indexOf('--port');
const gainIdx = args.indexOf('--gain');
if (portIdx === -1 || !args[portIdx + 1]) {
    console.error('Usage: npx ts-node extend-bounds.ts --port COM7 [--gain 40]');
    process.exit(1);
}
const options = {
    port: args[portIdx + 1],
    gain: gainIdx !== -1 ? parseInt(args[gainIdx + 1]) : undefined,
    outputDir: 'tuning-results',
};
runExtendedBoundsTest(options).catch(err => {
    console.error('Error:', err);
    process.exit(1);
});
