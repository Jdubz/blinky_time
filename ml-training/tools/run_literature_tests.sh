#!/bin/bash
# Run all 5 literature-validated A/B tests from NEXT_TESTS.md
# Each test: baseline (current defaults) vs literature values
# Results saved to ml-training/tools/results/literature_tests_$(date)/
#
# Run from blinkyhost: cd blinky_time/blinky-test-player && bash ../ml-training/tools/run_literature_tests.sh
#
# Uses 3 nRF52840 devices on ACM1/ACM2/ACM4.
# Each test plays all 18 EDM tracks TWICE (baseline + test), ~21 min per test.
# Total: ~1h 45min for all 5 tests.

set -eo pipefail

PORTS="/dev/ttyACM1,/dev/ttyACM2,/dev/ttyACM4"
RESULTS_DIR="../ml-training/tools/results/literature_tests_$(date +%Y%m%d_%H%M%S)"

# Reset all sweep params to defaults on exit (prevents contaminated state)
reset_all_params() {
  echo "Resetting all devices to defaults..."
  NODE_PATH=node_modules node -e "
const {SerialPort} = require('serialport');
async function reset() {
  for (const p of '$PORTS'.split(',')) {
    try {
      const s = new SerialPort({path: p, baudRate: 115200});
      await new Promise(r => s.on('open', r));
      s.write('set percival2 0.5\n');
      s.write('set percival4 0.25\n');
      s.write('set odfcontrast 1.25\n');
      s.write('set bassflux 0.5\n');
      s.write('set midflux 0.2\n');
      s.write('set highflux 0.3\n');
      s.write('set combfeedback 0.855\n');
      s.write('set rayleighbpm 130\n');
      await new Promise(r => setTimeout(r, 500));
      s.close();
    } catch(e) { console.error('Reset failed for ' + p + ': ' + e.message); }
  }
}
reset().then(() => process.exit(0));
" 2>&1 || echo "WARNING: Reset failed — check device params manually"
}
trap reset_all_params EXIT
mkdir -p "$RESULTS_DIR"

echo "============================================"
echo "  Literature-Validated A/B Tests"
echo "  Results: $RESULTS_DIR"
echo "  Ports: $PORTS"
echo "============================================"
echo ""

# Test #1: Percival Harmonic Weights (percival2=0.5/percival4=0.25 → 1.0/1.0)
# The A/B test script toggles one setting, so we sweep percival2 with percival4 locked.
# Run twice: once with percival4 at each value to get the full comparison.
echo "=== TEST 1: Percival Harmonic Weights ==="
echo "  Baseline: percival2=0.5, percival4=0.25 (current)"
echo "  Test: percival2=1.0, percival4=1.0 (literature)"
echo ""

# Sweep percival2 [0.5, 1.0] with percival4=1.0 (literature value for companion param)
NODE_PATH=node_modules node ../ml-training/tools/param_sweep_multidev.cjs \
  --param percival2 --min 0.5 --max 1.0 --steps 2 \
  --pre "percival4=1.0" \
  --ports "$PORTS" 2>&1 | tee "$RESULTS_DIR/test1_percival_p4lit.log"

# Also sweep with percival4=0.25 (current) to isolate percival2's effect
NODE_PATH=node_modules node ../ml-training/tools/param_sweep_multidev.cjs \
  --param percival2 --min 0.5 --max 1.0 --steps 2 \
  --pre "percival4=0.25" \
  --ports "$PORTS" 2>&1 | tee "$RESULTS_DIR/test1_percival_p4cur.log"

# Reset percival4 back to default
NODE_PATH=node_modules node -e "
const {SerialPort} = require('serialport');
async function reset() {
  for (const p of '$PORTS'.split(',')) {
    const s = new SerialPort({path: p, baudRate: 115200});
    await new Promise(r => s.on('open', r));
    s.write('set percival2 0.5\n');
    s.write('set percival4 0.25\n');
    await new Promise(r => setTimeout(r, 500));
    s.close();
  }
}
reset().then(() => process.exit(0));
" 2>/dev/null

echo ""
echo "=== TEST 2: ODF Contrast Exponent ==="
echo "  Sweep: odfcontrast [0.5, 1.0, 2.0] (current=2.0)"
echo ""

NODE_PATH=node_modules node ../ml-training/tools/param_sweep_multidev.cjs \
  --param odfcontrast --min 0.5 --max 2.0 --steps 3 \
  --ports "$PORTS" 2>&1 | tee "$RESULTS_DIR/test2_odfcontrast.log"

echo ""
echo "=== TEST 3: Spectral Flux Band Weights ==="
echo "  Baseline: bass=0.5, mid=0.2, high=0.3 (current)"
echo "  Test: bass=0.33, mid=0.33, high=0.33 (uniform/literature)"
echo ""

# Sweep bassflux [0.33, 0.5] with mid/high set to uniform
NODE_PATH=node_modules node ../ml-training/tools/param_sweep_multidev.cjs \
  --param bassflux --min 0.33 --max 0.5 --steps 2 \
  --pre "midflux=0.33,highflux=0.33" \
  --ports "$PORTS" 2>&1 | tee "$RESULTS_DIR/test3_bandweights_uniform.log"

# Also test with current mid/high to isolate bass effect
NODE_PATH=node_modules node ../ml-training/tools/param_sweep_multidev.cjs \
  --param bassflux --min 0.33 --max 0.5 --steps 2 \
  --pre "midflux=0.2,highflux=0.3" \
  --ports "$PORTS" 2>&1 | tee "$RESULTS_DIR/test3_bandweights_bassonly.log"

# Reset band weights
NODE_PATH=node_modules node -e "
const {SerialPort} = require('serialport');
async function reset() {
  for (const p of '$PORTS'.split(',')) {
    const s = new SerialPort({path: p, baudRate: 115200});
    await new Promise(r => s.on('open', r));
    s.write('set bassflux 0.5\n');
    s.write('set midflux 0.2\n');
    s.write('set highflux 0.3\n');
    await new Promise(r => setTimeout(r, 500));
    s.close();
  }
}
reset().then(() => process.exit(0));
" 2>/dev/null

echo ""
echo "=== TEST 4: Comb Feedback ==="
echo "  Sweep: combfeedback [0.79, 0.85, 0.92] (current=0.92)"
echo ""

NODE_PATH=node_modules node ../ml-training/tools/param_sweep_multidev.cjs \
  --param combfeedback --min 0.79 --max 0.92 --steps 3 \
  --ports "$PORTS" 2>&1 | tee "$RESULTS_DIR/test4_combfeedback.log"

echo ""
echo "=== TEST 5: Rayleigh Prior ==="
echo "  Sweep: rayleighbpm [120, 130, 140] (current=140)"
echo ""

NODE_PATH=node_modules node ../ml-training/tools/param_sweep_multidev.cjs \
  --param rayleighbpm --min 120 --max 140 --steps 3 \
  --ports "$PORTS" 2>&1 | tee "$RESULTS_DIR/test5_rayleighbpm.log"

echo ""
echo "============================================"
echo "  ALL TESTS COMPLETE"
echo "  Results: $RESULTS_DIR"
echo "============================================"
