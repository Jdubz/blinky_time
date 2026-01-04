# Blinky Time - Improvement Plan

*Last Updated: January 3, 2026*

## Current Status

### Completed (January 2026)

**Multi-Hypothesis Tempo Tracking (v3):**
- Multi-hypothesis tracking (4 concurrent tempos) implemented
- Handles tempo changes, ambiguity (half-time/double-time), and polyrhythmic patterns
- Confidence-based promotion with ≥8 beat requirement
- Dual decay strategy: beat-count (phrase-aware) + time-based (silence)
- SerialConsole commands: `show hypotheses`, `show primary`, `set hypodebug`
- Memory: +1 KB RAM, +4 KB program storage
- CPU: +3-4% @ 64 MHz

### Completed (December 2025)

**Architecture:**
- Generator → Effect → Renderer pattern implemented
- AudioController v2 with autocorrelation-based rhythm tracking (single-hypothesis)
- PLL-based phase tracking replaced with pattern analysis
- 5 transient detection algorithms: Drummer, Bass, HFC, Spectral, Hybrid

**Testing Infrastructure:**
- blinky-serial-mcp: MCP server for device communication (20+ tools)
- blinky-test-player: Audio pattern playback + ground truth generation
- param-tuner: Binary search + sweep optimization
- Comprehensive parameter tuning guide (56 tunable parameters)

**Calibration:**
- Fast-tune sessions completed
- Hybrid mode with equal weights (0.5/0.5) optimal
- Cooldown increased to 80ms for reduced false positives
- Parameters documented in PARAMETER_TUNING_HISTORY.md

**Documentation:**
- Architecture docs updated for AudioController v2
- Testing consolidated into AUDIO-TUNING-GUIDE.md
- Obsolete plans removed
- CLAUDE.md maintained for developer guidance

### In Progress

**Tuning Refinement:**
- [ ] Extended range testing (hitthresh, attackmult hit boundaries)
- [ ] Pad rejection improvement (high false positive rate)
- [ ] Fast-tempo optimization (drummer mode recall issues)

**Portfolio:**
- [ ] Demo video/GIFs for README
- [ ] Hardware photos of installations

---

## Technical Improvements

### Priority 1: Transient Detection (Short-term)

**Parameter Boundaries:**
Some parameters hit minimum bounds during fast-tune, suggesting optimal values may be outside tested range:

| Parameter | Tested Min | Optimal | Status |
|-----------|------------|---------|--------|
| hitthresh | 1.0 | 1.192 | Near boundary, needs retest with 0.5 min |
| attackmult | 1.0 | 1.1 | Near boundary, needs retest with 0.9 min |

**Known Issues:**
- pad-rejection: 50-229 false positives across modes
- simultaneous: Overlapping sounds detected as single event
- fast-tempo: 67% missed at high speed with drummer mode

### Priority 2: Hardware Validation (Medium-term)

Test all device configurations on actual hardware:

- [ ] **Tube Light (4x15 matrix)** - Primary test platform
- [ ] **Hat (89 LEDs, string)** - Sideways fire effect
- [ ] **Bucket Totem (16x8 matrix)** - Horizontal fire

### Priority 3: Runtime Configuration (Long-term)

- [ ] Dynamic device switching via serial
- [ ] EEPROM configuration persistence

---

## Next Actions

### Immediate (This Week)
1. Complete extended boundary testing with new parameter ranges
2. Run full validation suite and document results
3. Update PARAMETER_TUNING_HISTORY.md with findings

### Short-term (2-4 Weeks)
1. Hardware testing on actual LED strips
2. Demo video creation
3. Investigate algorithmic solutions for pad/simultaneous issues

### Long-term (1-2 Months)
1. Runtime configuration system
2. Mobile/web configuration app
3. Additional generator effects

---

## Architecture References

| Document | Purpose |
|----------|---------|
| [MUSIC_MODE_SIMPLIFIED.md](../MUSIC_MODE_SIMPLIFIED.md) | AudioController architecture |
| [docs/AUDIO-TUNING-GUIDE.md](AUDIO-TUNING-GUIDE.md) | Parameter reference + test procedures |
| [docs/GENERATOR_EFFECT_ARCHITECTURE.md](GENERATOR_EFFECT_ARCHITECTURE.md) | Generator design patterns |
| [blinky-test-player/PARAMETER_TUNING_HISTORY.md](../blinky-test-player/PARAMETER_TUNING_HISTORY.md) | Calibration history |
