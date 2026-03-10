#!/usr/bin/env node
/**
 * Analyze all CBSS parameter sweep results and generate optimal config.
 *
 * 1. Reads all sweep-*.json files from tuning-results/
 * 2. Finds best value for each parameter
 * 3. Generates a combined config JSON for config_test_multidev.cjs
 *
 * Usage:
 *   cd blinky-test-player && NODE_PATH=node_modules node ../ml-training/tools/analyze_all_sweeps.cjs
 */

const fs = require('fs');
const path = require('path');

const resultsDir = process.argv[2] || 'tuning-results';

// Find all sweep JSON files (skip the -latest.log files)
const sweepFiles = fs.readdirSync(resultsDir)
  .filter(f => f.startsWith('sweep-') && f.endsWith('.json'))
  .sort();

if (sweepFiles.length === 0) {
  console.error(`No sweep results found in ${resultsDir}/`);
  process.exit(1);
}

console.log(`\n${'='.repeat(90)}`);
console.log('COMBINED SWEEP ANALYSIS');
console.log(`${'='.repeat(90)}\n`);

const bestValues = {};
const defaultValues = {
  rayleighbpm: 120,
  postfloor: 0.05,
  octavescoreratio: 1.3,
  cbsscontrast: 1.0,  // Note: auto-overridden to 2.0 for NN
  cbssalpha: 0.9,     // Note: auto-overridden to 0.8 for NN
  octavecheckbeats: 2
};

for (const file of sweepFiles) {
  const data = JSON.parse(fs.readFileSync(path.join(resultsDir, file), 'utf-8'));

  // Compute stats for each value
  let bestScore = Infinity, bestValue = null;
  const rows = [];

  for (const sv of data.results) {
    const tracks = sv.tracks;
    const errors = tracks.filter(t => t.error !== null).map(t => t.error);
    const octaveCount = tracks.filter(t => t.octave).length;
    const meanErr = errors.length > 0 ? errors.reduce((a, b) => a + b) / errors.length : Infinity;
    const tracksOk = tracks.filter(t => !t.octave && t.error !== null && t.error < 10).length;

    // Score: mean error + 10 BPM penalty per octave error (matches analyze_sweep.cjs)
    const score = meanErr + octaveCount * 10;

    rows.push({ value: sv.value, meanErr, octaveCount, tracksOk, total: tracks.length, score });

    if (score < bestScore) {
      bestScore = score;
      bestValue = sv.value;
    }
  }

  const defaultVal = defaultValues[data.param] ?? '?';
  const improved = bestValue !== defaultVal;

  console.log(`--- ${data.param} (default: ${defaultVal}) ---`);
  console.log('  ' + 'Value'.padEnd(10) + 'Mean Err'.padEnd(10) + 'Oct Err'.padEnd(10) +
    'OK (<10)'.padEnd(10) + 'Score'.padEnd(10));

  for (const r of rows) {
    const marker = r.value === bestValue ? ' <<<' :
                   r.value == defaultVal ? ' (default)' : '';
    console.log('  ' +
      `${r.value}`.padEnd(10) +
      `${r.meanErr.toFixed(1)}`.padEnd(10) +
      `${r.octaveCount}/${r.total}`.padEnd(10) +
      `${r.tracksOk}/${r.total}`.padEnd(10) +
      `${r.score.toFixed(1)}${marker}`
    );
  }

  bestValues[data.param] = { best: bestValue, default: defaultVal, improved, bestScore };
  console.log(`  Best: ${bestValue} (score ${bestScore.toFixed(1)})${improved ? ' *** IMPROVED ***' : ' (no change)'}\n`);
}

// Summary
console.log(`${'='.repeat(90)}`);
console.log('OPTIMAL PARAMETER CONFIGURATION');
console.log(`${'='.repeat(90)}`);

const optimized = {};
let anyImproved = false;
for (const [param, info] of Object.entries(bestValues)) {
  const arrow = info.improved ? ` -> ${info.best}` : ' (keep default)';
  console.log(`  ${param}: ${info.default}${arrow}`);
  if (info.improved) {
    optimized[param] = info.best;
    anyImproved = true;
  }
}

if (!anyImproved) {
  console.log('\nNo parameters improved over defaults. Current configuration may be near-optimal.');
  process.exit(0);
}

// Generate config file for config_test_multidev.cjs
// Tests: baseline (defaults) vs optimized (best values from sweep)
const configTest = {
  description: 'CBSS parameter sweep optimization: baseline vs optimized',
  timestamp: new Date().toISOString(),
  configs: [
    { _label: 'baseline', ...Object.fromEntries(Object.entries(bestValues).map(([k, v]) => [k, v.default])) },
    { _label: 'optimized', ...Object.fromEntries(Object.entries(bestValues).map(([k, v]) => [k, v.best])) }
  ]
};

const outPath = path.join(resultsDir, 'cbss-optimized-config.json');
fs.writeFileSync(outPath, JSON.stringify(configTest, null, 2));
console.log(`\nConfig file for A/B test: ${outPath}`);
console.log('Run: node ../ml-training/tools/config_test_multidev.cjs --configs ' + outPath);
