const hit = require('./tuning-results/sweeps/hitthresh.json');
const flux = require('./tuning-results/sweeps/fluxthresh.json');

console.log('═══════════════════════════════════════════════════');
console.log('HITTHRESH (Drummer Mode) - Current default: 2.0');
console.log('═══════════════════════════════════════════════════');
console.log('Value  | Avg F1 | Precision | Recall');
console.log('-------|--------|-----------|--------');
hit.sweep.forEach(s => {
  const mark = s.value === hit.optimal.value ? ' ★' : '  ';
  console.log(`${s.value.toString().padEnd(6)}${mark}| ${s.avgF1.toFixed(3)} | ${s.avgPrecision.toFixed(3)}    | ${s.avgRecall.toFixed(3)}`);
});
console.log(`\nOptimal: ${hit.optimal.value} (F1: ${hit.optimal.avgF1.toFixed(3)})`);

console.log('\n═══════════════════════════════════════════════════');
console.log('FLUXTHRESH (Spectral Mode) - Current default: 2.8');
console.log('═══════════════════════════════════════════════════');
console.log('Value  | Avg F1 | Precision | Recall');
console.log('-------|--------|-----------|--------');
flux.sweep.forEach(s => {
  const mark = s.value === flux.optimal.value ? ' ★' : '  ';
  console.log(`${s.value.toString().padEnd(6)}${mark}| ${s.avgF1.toFixed(3)} | ${s.avgPrecision.toFixed(3)}    | ${s.avgRecall.toFixed(3)}`);
});
console.log(`\nOptimal: ${flux.optimal.value} (F1: ${flux.optimal.avgF1.toFixed(3)})`);

console.log('\n═══════════════════════════════════════════════════');
console.log('ANALYSIS');
console.log('═══════════════════════════════════════════════════');
console.log('hitthresh: 2.0 → 3.5 (improve F1 from ~0.44 to 0.46)');
console.log('fluxthresh: 2.8 → 2.0 (improve F1 from ~0.59 to 0.67)');
console.log('\nBoth changes improve detection performance!');
