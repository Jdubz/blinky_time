#!/usr/bin/env node
/**
 * Generate a Definitive Screening Design (DSD) for parameter optimization.
 *
 * DSD requires only 2k+1 runs for k factors (Jones & Nachtsheim 2011).
 * Identifies main effects AND two-factor interactions in one experiment.
 *
 * Output: a set of param_sweep_multidev.cjs commands to run the design.
 *
 * Usage:
 *   node generate_screening_design.cjs
 *   node generate_screening_design.cjs --output screening-design.json
 */

const fs = require('fs');
const args = process.argv.slice(2);
function getArg(name, defaultValue) {
  const idx = args.indexOf(name);
  if (idx === -1 || idx + 1 >= args.length) return defaultValue;
  return args[idx + 1];
}

const outputFile = getArg('--output', '');

// Define the parameters to screen
// Each has: name, min (low), center, max (high), settingName (for serial command)
const factors = [
  { name: 'fwdfiltlambda',   min: 4,    center: 10,   max: 16,   setting: 'fwdfiltlambda' },
  { name: 'fwdfiltcontrast', min: 1.0,  center: 3.0,  max: 5.0,  setting: 'fwdfiltcontrast' },
  { name: 'fwdtranssigma',   min: 0.5,  center: 3.0,  max: 6.0,  setting: 'fwdtranssigma' },
  { name: 'fwdbayesbias',    min: 0.0,  center: 0.5,  max: 1.0,  setting: 'fwdbayesbias' },
  { name: 'fwdasymmetry',    min: 0.0,  center: 2.0,  max: 4.0,  setting: 'fwdasymmetry' },
  { name: 'fwdfiltfloor',    min: 0.001, center: 0.01, max: 0.05, setting: 'fwdfiltfloor' },
];

const k = factors.length; // 6 factors

// DSD Conference Matrix for k=6 (even)
// Each row has entries from {-1, 0, +1}
// The design is: k rows from conference matrix C, k rows from -C, plus one center point
// For k=6 (even), we use a conference matrix of order 6.
// Conference matrix: C is k×k with 0 on diagonal and ±1 elsewhere,
// such that C'C = (k-1)I. For practical DSDs, we use a known construction.
//
// DSD for 6 factors = 2*6 + 1 = 13 runs
const dsdMatrix = [
  // Rows from conference matrix C (fold 1)
  [ 0, +1, +1, +1, +1, +1],
  [+1,  0, +1, -1, -1, +1],
  [+1, +1,  0, +1, -1, -1],
  [+1, -1, +1,  0, +1, -1],
  [+1, -1, -1, +1,  0, +1],
  [+1, +1, -1, -1, +1,  0],
  // Foldover rows: -C
  [ 0, -1, -1, -1, -1, -1],
  [-1,  0, -1, +1, +1, -1],
  [-1, -1,  0, -1, +1, +1],
  [-1, +1, -1,  0, -1, +1],
  [-1, +1, +1, -1,  0, -1],
  [-1, -1, +1, +1, -1,  0],
  // Center point
  [ 0,  0,  0,  0,  0,  0],
];

// Convert coded levels to actual values
function codeToValue(factor, code) {
  if (code === -1) return factor.min;
  if (code === 0) return factor.center;
  if (code === +1) return factor.max;
  return factor.center;
}

console.log(`Definitive Screening Design for ${k} factors`);
console.log(`Runs required: ${dsdMatrix.length} (2k+1 = ${2*k+1})`);
console.log(`Estimated time: ${dsdMatrix.length} runs × 18 tracks × 40s = ${Math.round(dsdMatrix.length * 18 * 40 / 3600 * 10) / 10} hours\n`);

console.log('Factors:');
for (const f of factors) {
  console.log(`  ${f.name.padEnd(20)} min=${f.min}  center=${f.center}  max=${f.max}`);
}

console.log(`\n${'Run'.padEnd(5)}${factors.map(f => f.name.substring(0, 15).padEnd(17)).join('')}`);
console.log('-'.repeat(5 + factors.length * 17));

const runs = [];
for (let r = 0; r < dsdMatrix.length; r++) {
  const row = dsdMatrix[r];
  const values = {};
  let line = `${(r + 1).toString().padEnd(5)}`;
  for (let j = 0; j < factors.length; j++) {
    const val = codeToValue(factors[j], row[j]);
    values[factors[j].setting] = val;
    line += `${val.toString().padEnd(17)}`;
  }
  runs.push(values);
  console.log(line);
}

// Generate shell commands for running on blinkyhost
console.log(`\n${'='.repeat(80)}`);
console.log('EXECUTION COMMANDS (run from blinky-test-player/ on blinkyhost):');
console.log('='.repeat(80));
console.log('# Each run plays all 18 tracks with the given parameter config.');
console.log('# Total time: ~2.6 hours. Run sequentially.\n');

// For screening, we can't use param_sweep (it sweeps one param).
// We need a new approach: set all params, play all tracks, collect BPM.
// The ab_test_multidev.cjs can be adapted, or we use a wrapper.
console.log('# Option 1: Use ab_test_multidev.cjs with --pre for each config');
console.log('# (set fwdfilter=1, then set all params via --pre)\n');

for (let r = 0; r < runs.length; r++) {
  const config = runs[r];
  const preStr = Object.entries(config)
    .map(([k, v]) => `${k}=${v}`)
    .join(',');
  console.log(`# Run ${r + 1}/${runs.length}`);
  console.log(`NODE_PATH=node_modules node ../ml-training/tools/ab_test_multidev.cjs \\`);
  console.log(`  --setting fwdfilter --pre "${preStr}" \\`);
  console.log(`  --music-dir music/edm --duration 35000\n`);
}

// Save design as JSON
if (outputFile) {
  const design = {
    type: 'definitive_screening_design',
    factors: factors,
    nRuns: dsdMatrix.length,
    codedMatrix: dsdMatrix,
    runs: runs,
    estimatedHours: Math.round(dsdMatrix.length * 18 * 40 / 3600 * 10) / 10,
  };
  fs.writeFileSync(outputFile, JSON.stringify(design, null, 2));
  console.log(`\nDesign saved to ${outputFile}`);
}

// Explain what to do with results
console.log('\n' + '='.repeat(80));
console.log('ANALYSIS:');
console.log('='.repeat(80));
console.log('After collecting results from all 13 runs:');
console.log('1. For each run, compute mean BPM error and octave error count');
console.log('2. Fit a response surface model: Y = b0 + sum(bi*Xi) + sum(bii*Xi^2) + sum(bij*Xi*Xj)');
console.log('3. Main effects: large |bi| indicates parameter i strongly affects performance');
console.log('4. Interactions: large |bij| indicates parameters i and j interact');
console.log('5. Curvature: large |bii| indicates non-linear relationship');
console.log('6. Focus optimization on parameters with large main effects');
console.log('7. Jointly optimize parameters with significant interactions');
