#!/usr/bin/env node
/**
 * Analyze parameter sweep results from param_sweep_multidev.cjs
 *
 * Usage:
 *   node analyze_sweep.cjs tuning-results/sweep-fwdfiltlambda-*.json
 *   node analyze_sweep.cjs tuning-results/sweep-*.json   # compare multiple sweeps
 */

const fs = require('fs');
const path = require('path');

const files = process.argv.slice(2);
if (files.length === 0) {
  console.error('Usage: node analyze_sweep.cjs <sweep-results.json> [...]');
  process.exit(1);
}

for (const file of files) {
  if (!fs.existsSync(file)) {
    console.error(`File not found: ${file}`);
    continue;
  }

  const data = JSON.parse(fs.readFileSync(file, 'utf-8'));
  console.log(`\n${'='.repeat(100)}`);
  console.log(`SWEEP: ${data.param} [${data.min} -> ${data.max}] (${data.steps} steps)`);
  console.log(`Date: ${data.timestamp}, Devices: ${data.nDevices}, Duration: ${data.durationMs}ms, Settle: ${data.settleMs}ms`);
  if (data.enableSettings) console.log(`Enable: ${data.enableSettings}`);
  if (data.preSettings) console.log(`Pre: ${data.preSettings}`);
  console.log('='.repeat(100));

  // Compute per-value statistics
  const stats = [];
  for (const sv of data.results) {
    const tracks = sv.tracks;
    const errors = tracks.filter(t => t.error !== null).map(t => t.error);
    const octaveCount = tracks.filter(t => t.octave).length;
    const meanErr = errors.length > 0 ? errors.reduce((a, b) => a + b) / errors.length : Infinity;
    const medianErr = errors.length > 0 ? sortedMedian(errors) : Infinity;
    const maxErr = errors.length > 0 ? Math.max(...errors) : Infinity;
    const tracksOk = tracks.filter(t => !t.octave && t.error !== null && t.error < 10).length;
    const tracksGood = tracks.filter(t => !t.octave && t.error !== null && t.error < 5).length;

    // Composite score: mean error + 10 BPM penalty per octave error
    const score = meanErr + octaveCount * 10;

    stats.push({
      value: sv.value,
      meanErr, medianErr, maxErr,
      octaveCount, tracksOk, tracksGood,
      total: tracks.length, score, tracks
    });
  }

  // Summary table
  console.log('\n' + 'Value'.padEnd(10) + 'Mean Err'.padEnd(10) + 'Med Err'.padEnd(10) +
    'Max Err'.padEnd(10) + 'Oct Err'.padEnd(10) + '<10 BPM'.padEnd(10) +
    '<5 BPM'.padEnd(10) + 'Score'.padEnd(10));
  console.log('-'.repeat(80));

  let bestScore = Infinity, bestValue = null;
  for (const s of stats) {
    if (s.score < bestScore) { bestScore = s.score; bestValue = s.value; }
    const marker = s.score <= bestScore ? ' *' : '';
    console.log(
      `${s.value}`.padEnd(10) +
      `${s.meanErr.toFixed(1)}`.padEnd(10) +
      `${s.medianErr.toFixed(1)}`.padEnd(10) +
      `${s.maxErr.toFixed(1)}`.padEnd(10) +
      `${s.octaveCount}/${s.total}`.padEnd(10) +
      `${s.tracksOk}/${s.total}`.padEnd(10) +
      `${s.tracksGood}/${s.total}`.padEnd(10) +
      `${s.score.toFixed(1)}${marker}`
    );
  }

  console.log('-'.repeat(80));
  console.log(`BEST: ${data.param} = ${bestValue} (score ${bestScore.toFixed(1)})`);

  // Per-track breakdown: show which tracks are problematic across values
  console.log(`\nPER-TRACK BREAKDOWN:`);
  const trackNames = stats[0].tracks.map(t => t.track);
  const hdr = 'Track'.padEnd(30) + 'True BPM'.padEnd(10) +
    stats.map(s => `${s.value}`.padEnd(12)).join('');
  console.log(hdr);
  console.log('-'.repeat(30 + 10 + stats.length * 12));

  for (let ti = 0; ti < trackNames.length; ti++) {
    const name = trackNames[ti];
    const trueBpm = stats[0].tracks[ti].trueBpm;
    let row = name.substring(0, 28).padEnd(30) +
      (trueBpm ? trueBpm.toFixed(0) : '?').padEnd(10);
    for (const s of stats) {
      const t = s.tracks[ti];
      const errStr = t.error !== null ? t.error.toFixed(1) : '?';
      const octStr = t.octave ? '!' : ' ';
      row += `${errStr}${octStr}`.padEnd(12);
    }
    console.log(row);
  }
  console.log('\n(! = octave error)');

  // Monotonicity analysis: is the error monotonically related to the parameter?
  const meanErrs = stats.map(s => s.meanErr);
  const isMonoDecr = meanErrs.every((v, i) => i === 0 || v <= meanErrs[i - 1] + 0.5);
  const isMonoIncr = meanErrs.every((v, i) => i === 0 || v >= meanErrs[i - 1] - 0.5);
  if (isMonoDecr) console.log(`\nTrend: Mean error DECREASES as ${data.param} increases -> sweep range may need extension upward`);
  else if (isMonoIncr) console.log(`\nTrend: Mean error INCREASES as ${data.param} increases -> sweep range may need extension downward`);
  else console.log(`\nTrend: Non-monotonic -> optimum appears to be within sweep range`);

  // Check if best is at boundary
  if (bestValue === data.sweepValues[0]) {
    console.log(`WARNING: Best value is at lower boundary (${data.min}). Consider extending sweep range lower.`);
  } else if (bestValue === data.sweepValues[data.sweepValues.length - 1]) {
    console.log(`WARNING: Best value is at upper boundary (${data.max}). Consider extending sweep range higher.`);
  }
}

function sortedMedian(arr) {
  const sorted = [...arr].sort((a, b) => a - b);
  const mid = Math.floor(sorted.length / 2);
  return sorted.length % 2 === 0 ? (sorted[mid - 1] + sorted[mid]) / 2 : sorted[mid];
}
