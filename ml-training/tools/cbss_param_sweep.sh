#!/bin/bash
# CBSS parameter sweep for NN ODF optimization
set -eo pipefail

cd "$(dirname "$0")/../../blinky-test-player"
export NODE_PATH=node_modules
SWEEP="node ../ml-training/tools/param_sweep_multidev.cjs"
RESULTS_DIR="tuning-results"
mkdir -p "$RESULTS_DIR"

echo "=============================================="
echo "CBSS Parameter Sweep Suite"
echo "Started: $(date)"
echo "=============================================="

# 1. rayleighBpm: main Rayleigh prior bias (default 120)
echo -e "\n\n>>> SWEEP 1/6: rayleighBpm [80-180]"
$SWEEP --param rayleighbpm --min 80 --max 180 --steps 6 2>&1 | tee "$RESULTS_DIR/sweep-rayleighbpm-latest.log"

# 2. posteriorFloor: mode escape (default 0.05)
echo -e "\n\n>>> SWEEP 2/6: posteriorFloor [0.03-0.20]"
$SWEEP --param postfloor --min 0.03 --max 0.20 --steps 6 2>&1 | tee "$RESULTS_DIR/sweep-postfloor-latest.log"

# 3. octaveScoreRatio: octave switch threshold (default 1.3)
echo -e "\n\n>>> SWEEP 3/6: octaveScoreRatio [1.05-2.0]"
$SWEEP --param octavescoreratio --min 1.05 --max 2.0 --steps 6 2>&1 | tee "$RESULTS_DIR/sweep-octavescoreratio-latest.log"

# 4. cbssContrast: NN ODF contrast (auto 2.0)
echo -e "\n\n>>> SWEEP 4/6: cbssContrast [0.5-5.0]"
$SWEEP --param cbsscontrast --min 0.5 --max 5.0 --steps 6 2>&1 | tee "$RESULTS_DIR/sweep-cbsscontrast-latest.log"

# 5. cbssAlpha: NN CBSS momentum (auto 0.8)
echo -e "\n\n>>> SWEEP 5/6: cbssAlpha [0.60-0.92]"
$SWEEP --param cbssalpha --min 0.60 --max 0.92 --steps 6 2>&1 | tee "$RESULTS_DIR/sweep-cbssalpha-latest.log"

# 6. octaveCheckBeats: check frequency (default 2)
echo -e "\n\n>>> SWEEP 6/6: octaveCheckBeats [1-4]"
$SWEEP --param octavecheckbeats --min 1 --max 4 --steps 4 2>&1 | tee "$RESULTS_DIR/sweep-octavecheckbeats-latest.log"

echo -e "\n=============================================="
echo "ALL SWEEPS COMPLETE: $(date)"
echo "=============================================="
