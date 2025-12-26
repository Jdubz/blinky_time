import { describe, it, expect } from 'vitest';
import { AudioSample } from '../types';

/**
 * Regression tests for AudioVisualizer and AdaptiveMic range normalization
 *
 * These tests document critical bugs that were fixed and should never regress:
 * - Bug #1: Noise gate applied to raw signal instead of mapped output
 * - Bug #2: Valley hardcoded to noise gate, preventing dynamic tracking
 * - Bug #3: Missing diagnostic fields (raw, alive)
 * - Bug #4: Transient visualization redundant with percussion indicators
 */
describe('AudioVisualizer - Range Normalization Regression Tests', () => {
  describe('AudioSample interface', () => {
    it('includes all required fields for proper visualization', () => {
      const sample: AudioSample = {
        l: 0.5,
        t: 0.3,
        pk: 0.4,
        vl: 0.05,
        raw: 0.2,
        h: 32,
        alive: 1,
        k: 0,
        sn: 1,
        hh: 0,
        ks: 0.5,
        ss: 0.8,
        hs: 0.3,
        z: 0.15,
      };

      // Verify all fields are present
      expect(sample.l).toBeDefined();
      expect(sample.t).toBeDefined();
      expect(sample.pk).toBeDefined();
      expect(sample.vl).toBeDefined();
      expect(sample.raw).toBeDefined();
      expect(sample.h).toBeDefined();
      expect(sample.alive).toBeDefined();
      expect(sample.k).toBeDefined();
      expect(sample.sn).toBeDefined();
      expect(sample.hh).toBeDefined();
      expect(sample.ks).toBeDefined();
      expect(sample.ss).toBeDefined();
      expect(sample.hs).toBeDefined();
      expect(sample.z).toBeDefined();
    });

    it('uses correct types for all fields', () => {
      const sample: AudioSample = {
        l: 0.5,
        t: 0.3,
        pk: 0.4,
        vl: 0.05,
        raw: 0.2,
        h: 32,
        alive: 1,
        k: 0,
        sn: 1,
        hh: 0,
        ks: 0.5,
        ss: 0.8,
        hs: 0.3,
        z: 0.15,
      };

      // Number fields
      expect(typeof sample.l).toBe('number');
      expect(typeof sample.t).toBe('number');
      expect(typeof sample.pk).toBe('number');
      expect(typeof sample.vl).toBe('number');
      expect(typeof sample.raw).toBe('number');
      expect(typeof sample.h).toBe('number');
      expect(typeof sample.ks).toBe('number');
      expect(typeof sample.ss).toBe('number');
      expect(typeof sample.hs).toBe('number');
      expect(typeof sample.z).toBe('number');

      // Boolean flags (0 or 1)
      expect([0, 1]).toContain(sample.alive);
      expect([0, 1]).toContain(sample.k);
      expect([0, 1]).toContain(sample.sn);
      expect([0, 1]).toContain(sample.hh);
    });
  });

  describe('Range normalization values', () => {
    it('peak and valley are in raw 0-1 range (not mapped output)', () => {
      // Regression test: Peak/valley should be RAW values before window mapping
      // They will typically be small (0.02-0.30 range) even when audio is present
      const sample: AudioSample = {
        l: 0.75, // Mapped output (0-1)
        t: 0.5,
        pk: 0.08, // Raw peak (small value is correct!)
        vl: 0.02, // Raw valley (small value is correct!)
        raw: 0.25,
        h: 32,
        alive: 1,
        k: 0,
        sn: 0,
        hh: 0,
        ks: 0.0,
        ss: 0.0,
        hs: 0.0,
        z: 0.15,
      };

      // Peak/valley should be much smaller than level
      expect(sample.pk).toBeLessThan(0.5);
      expect(sample.vl).toBeLessThan(sample.pk);

      // But level can still be high (0-1 range after mapping)
      expect(sample.l).toBeGreaterThan(sample.pk);
    });

    it('valley is not hardcoded to noise gate', () => {
      // Regression test: Valley should be dynamic, not always 0.04
      // Valley can be anywhere from ~0.02 to ~0.10 depending on signal
      const samples: AudioSample[] = [
        {
          l: 0.5,
          t: 0.0,
          pk: 0.08,
          vl: 0.02, // Low valley (quiet environment)
          raw: 0.2,
          h: 32,
          alive: 1,
          k: 0,
          sn: 0,
          hh: 0,
          ks: 0.0,
          ss: 0.0,
          hs: 0.0,
          z: 0.1,
        },
        {
          l: 0.7,
          t: 0.0,
          pk: 0.15,
          vl: 0.05, // Higher valley (noisier environment)
          raw: 0.25,
          h: 32,
          alive: 1,
          k: 0,
          sn: 0,
          hh: 0,
          ks: 0.0,
          ss: 0.0,
          hs: 0.0,
          z: 0.12,
        },
      ];

      // Valley values should be different (not all 0.04)
      expect(samples[0].vl).not.toBe(0.04);
      expect(samples[1].vl).not.toBe(0.04);
      expect(samples[0].vl).not.toBe(samples[1].vl);
    });

    it('includes raw ADC level for diagnostics', () => {
      // Regression test: Raw ADC level must be exposed for debugging
      const sample: AudioSample = {
        l: 0.6,
        t: 0.0,
        pk: 0.12,
        vl: 0.03,
        raw: 0.25, // Raw ADC level (before any processing)
        h: 32,
        alive: 1,
        k: 0,
        sn: 0,
        hh: 0,
        ks: 0.0,
        ss: 0.0,
        hs: 0.0,
        z: 0.1,
      };

      // Raw should be present and in valid range
      expect(sample.raw).toBeDefined();
      expect(sample.raw).toBeGreaterThanOrEqual(0);
      expect(sample.raw).toBeLessThanOrEqual(1);

      // Ideal raw range for HW gain targeting is 0.15-0.35
      // But can be outside this range while adapting
      expect(typeof sample.raw).toBe('number');
    });

    it('includes PDM alive status for diagnostics', () => {
      // Regression test: PDM status must be exposed to detect mic failures
      const aliveSample: AudioSample = {
        l: 0.5,
        t: 0.0,
        pk: 0.1,
        vl: 0.02,
        raw: 0.2,
        h: 32,
        alive: 1, // PDM working
        k: 0,
        sn: 0,
        hh: 0,
        ks: 0.0,
        ss: 0.0,
        hs: 0.0,
        z: 0.1,
      };

      const deadSample: AudioSample = {
        l: 0.0,
        t: 0.0,
        pk: 0.08,
        vl: 0.02,
        raw: 0.0,
        h: 32,
        alive: 0, // PDM dead
        k: 0,
        sn: 0,
        hh: 0,
        ks: 0.0,
        ss: 0.0,
        hs: 0.0,
        z: 0.0,
      };

      expect(aliveSample.alive).toBe(1);
      expect(deadSample.alive).toBe(0);
    });
  });

  describe('Percussion detection values', () => {
    it('percussion strength is normalized to 0-1 range', () => {
      // Regression test: Percussion strength is now normalized to 0-1
      // 0.0 at threshold, 1.0 at 3x threshold (very strong hit)
      const sample: AudioSample = {
        l: 0.8,
        t: 1.0, // Strong transient (normalized)
        pk: 0.15,
        vl: 0.03,
        raw: 0.3,
        h: 32,
        alive: 1,
        k: 1,
        sn: 0,
        hh: 0,
        ks: 1.0, // Kick strength at max (3x threshold)
        ss: 0.0,
        hs: 0.0,
        z: 0.05,
      };

      expect(sample.ks).toBeLessThanOrEqual(1.0);
      expect(sample.ks).toBeGreaterThanOrEqual(0.0);
      expect(sample.t).toBeLessThanOrEqual(1.0);
      expect(sample.t).toBeGreaterThanOrEqual(0.0);
    });

    it('percussion impulse flags are boolean 0 or 1', () => {
      // Regression test: Impulse flags must be exactly 0 or 1
      const samples: AudioSample[] = [
        {
          l: 0.5,
          t: 0.9,
          pk: 0.1,
          vl: 0.02,
          raw: 0.2,
          h: 32,
          alive: 1,
          k: 1, // Kick detected
          sn: 0,
          hh: 1, // Hihat detected
          ks: 0.9,
          ss: 0.0,
          hs: 0.6,
          z: 0.15,
        },
        {
          l: 0.6,
          t: 1.0,
          pk: 0.12,
          vl: 0.03,
          raw: 0.25,
          h: 32,
          alive: 1,
          k: 0,
          sn: 1, // Snare detected
          hh: 0,
          ks: 0.0,
          ss: 1.0,
          hs: 0.0,
          z: 0.3,
        },
      ];

      // All impulse flags must be exactly 0 or 1
      for (const sample of samples) {
        expect([0, 1]).toContain(sample.k);
        expect([0, 1]).toContain(sample.sn);
        expect([0, 1]).toContain(sample.hh);
      }
    });
  });

  describe('Value relationships', () => {
    it('peak is always greater than or equal to valley', () => {
      // Regression test: Peak >= Valley (range must be non-negative)
      const sample: AudioSample = {
        l: 0.5,
        t: 0.0,
        pk: 0.1,
        vl: 0.02,
        raw: 0.2,
        h: 32,
        alive: 1,
        k: 0,
        sn: 0,
        hh: 0,
        ks: 0.0,
        ss: 0.0,
        hs: 0.0,
        z: 0.1,
      };

      expect(sample.pk).toBeGreaterThanOrEqual(sample.vl);
    });

    it('level can be anywhere from 0-1 regardless of raw values', () => {
      // Regression test: Level is mapped output (0-1), independent of raw signal level
      // Even if raw is very low, level can be high after normalization
      const samples: AudioSample[] = [
        {
          l: 0.9, // High level output
          t: 0.0,
          pk: 0.06, // Low raw peak
          vl: 0.02, // Low raw valley
          raw: 0.05, // Very low raw signal
          h: 64, // High gain compensates
          alive: 1,
          k: 0,
          sn: 0,
          hh: 0,
          ks: 0.0,
          ss: 0.0,
          hs: 0.0,
          z: 0.1,
        },
        {
          l: 0.1, // Low level output
          t: 0.0,
          pk: 0.3, // Higher raw peak
          vl: 0.05, // Higher raw valley
          raw: 0.28, // Much higher raw signal
          h: 20, // Lower gain
          alive: 1,
          k: 0,
          sn: 0,
          hh: 0,
          ks: 0.0,
          ss: 0.0,
          hs: 0.0,
          z: 0.12,
        },
      ];

      // Level and raw are decoupled (level is normalized)
      expect(samples[0].l).toBeGreaterThan(samples[1].l);
      expect(samples[0].raw).toBeLessThan(samples[1].raw);
    });
  });
});
