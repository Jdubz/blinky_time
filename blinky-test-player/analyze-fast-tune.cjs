const results = require('./tuning-results/fast-tune-results.json');

console.log('═══════════════════════════════════════════════════');
console.log('FAST-TUNE PATTERN PERFORMANCE ANALYSIS');
console.log('═══════════════════════════════════════════════════\n');

['drummer', 'spectral', 'hybrid'].forEach(mode => {
  const data = results.results.find(x => x.mode === mode);
  if (!data) return;

  console.log(`${mode.toUpperCase()} MODE (F1: ${data.f1.toFixed(3)})`);
  console.log('─'.repeat(60));

  const patterns = Object.entries(data.byPattern).sort((a, b) => a[1].f1 - b[1].f1);

  patterns.forEach(([name, stats]) => {
    const recall = (stats.recall * 100).toFixed(0);
    const precision = (stats.precision * 100).toFixed(0);
    const missed = ((1 - stats.recall) * 100).toFixed(0);

    console.log(`  ${name.padEnd(20)} F1=${stats.f1.toFixed(3)} P=${precision}% R=${recall}% (missed ${missed}%)`);
    console.log(`                       TP=${stats.truePositives} FP=${stats.falsePositives} FN=${stats.falseNegatives}/${stats.expectedTotal}`);
  });

  console.log('');
});

// Identify problem patterns
console.log('═══════════════════════════════════════════════════');
console.log('PROBLEM PATTERNS (F1 < 0.6 in any mode)');
console.log('═══════════════════════════════════════════════════\n');

const allPatterns = new Set();
results.results.forEach(r => {
  Object.keys(r.byPattern).forEach(p => allPatterns.add(p));
});

allPatterns.forEach(pattern => {
  const scores = results.results.map(r => ({
    mode: r.mode,
    f1: r.byPattern[pattern]?.f1 || 0,
    recall: r.byPattern[pattern]?.recall || 0,
    precision: r.byPattern[pattern]?.precision || 0
  }));

  const minF1 = Math.min(...scores.map(s => s.f1));
  if (minF1 < 0.6) {
    console.log(`${pattern}:`);
    scores.forEach(s => {
      const flag = s.f1 < 0.6 ? ' ⚠️ ' : '    ';
      console.log(`  ${flag}${s.mode.padEnd(10)} F1=${s.f1.toFixed(3)} P=${(s.precision*100).toFixed(0)}% R=${(s.recall*100).toFixed(0)}%`);
    });
    console.log('');
  }
});

// Boundary analysis
console.log('═══════════════════════════════════════════════════');
console.log('PARAMETER BOUNDARY ANALYSIS');
console.log('═══════════════════════════════════════════════════\n');

results.results.forEach(r => {
  console.log(`${r.mode.toUpperCase()}:`);
  Object.entries(r.params).forEach(([param, value]) => {
    console.log(`  ${param.padEnd(15)} = ${value}`);
  });
  console.log('');
});

console.log('BOUNDARY WARNINGS:');
console.log('  attackmult: 1.1 was AT minimum (1.1) - extended to 1.0');
console.log('  hitthresh: 1.688 was near minimum (1.5) - extended to 1.0');
console.log('  fluxthresh: 1.4 was near minimum (1.0) - extended to 0.5');
console.log('\nRecommend testing extended ranges to confirm no better values exist below.');
