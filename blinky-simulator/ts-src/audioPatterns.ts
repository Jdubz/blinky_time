/**
 * Audio Patterns - Scripted audio control sequences for simulation
 */

import { AudioControl } from './types';

export interface AudioPattern {
  name: string;
  duration: number;  // in ms
  getAudioAt(timeMs: number): AudioControl;
}

// Silent pattern - no audio
function createSilencePattern(durationMs: number): AudioPattern {
  return {
    name: 'silence',
    duration: durationMs,
    getAudioAt(_timeMs: number): AudioControl {
      return {
        energy: 0.1,
        pulse: 0,
        phase: 0,
        rhythmStrength: 0
      };
    }
  };
}

// Steady BPM pattern
function createBpmPattern(bpm: number, durationMs: number): AudioPattern {
  const beatIntervalMs = 60000 / bpm;

  return {
    name: `steady-${bpm}bpm`,
    duration: durationMs,
    getAudioAt(timeMs: number): AudioControl {
      const beatPhase = (timeMs % beatIntervalMs) / beatIntervalMs;
      const onBeat = beatPhase < 0.1;  // Beat pulse in first 10% of interval

      return {
        energy: 0.4 + Math.sin(timeMs / 500) * 0.1,  // Slight energy variation
        pulse: onBeat ? 0.9 : 0,
        phase: beatPhase,
        rhythmStrength: 0.8
      };
    }
  };
}

// Burst pattern - random transient bursts
function createBurstPattern(durationMs: number): AudioPattern {
  // Pre-generate burst times for reproducibility
  const bursts: number[] = [];
  let t = 0;
  while (t < durationMs) {
    t += 200 + Math.random() * 800;  // 200-1000ms between bursts
    if (t < durationMs) bursts.push(t);
  }

  return {
    name: 'burst',
    duration: durationMs,
    getAudioAt(timeMs: number): AudioControl {
      // Check if we're near a burst
      let pulse = 0;
      for (const burstTime of bursts) {
        const diff = Math.abs(timeMs - burstTime);
        if (diff < 50) {
          pulse = Math.max(pulse, 1 - diff / 50);
        }
      }

      return {
        energy: 0.3 + pulse * 0.3,
        pulse,
        phase: (timeMs / 500) % 1,
        rhythmStrength: 0.2
      };
    }
  };
}

// Complex pattern - building rhythm
function createComplexPattern(durationMs: number): AudioPattern {
  return {
    name: 'complex',
    duration: durationMs,
    getAudioAt(timeMs: number): AudioControl {
      // Build up over time
      const progress = timeMs / durationMs;

      // Base rhythm at 120 BPM
      const beatInterval = 500;  // 120 BPM
      const beatPhase = (timeMs % beatInterval) / beatInterval;

      // Add accent on every 4th beat
      const measurePhase = (timeMs % (beatInterval * 4)) / (beatInterval * 4);
      const accent = measurePhase < 0.025 ? 0.3 : 0;

      // Energy builds up
      const baseEnergy = 0.2 + progress * 0.4;

      // Pulse on beat, stronger as we progress
      const onBeat = beatPhase < 0.1;
      const pulseStrength = onBeat ? (0.5 + progress * 0.5) : 0;

      return {
        energy: baseEnergy + accent,
        pulse: pulseStrength + accent,
        phase: beatPhase,
        rhythmStrength: 0.3 + progress * 0.5
      };
    }
  };
}

export type PatternType = 'silence' | 'steady-90bpm' | 'steady-120bpm' | 'steady-140bpm' | 'fast' | 'burst' | 'bursts' | 'complex' | 'silent';

export function createPattern(type: PatternType | string, durationMs: number): AudioPattern {
  switch (type) {
    case 'silence':
    case 'silent':
      return createSilencePattern(durationMs);

    case 'steady-90bpm':
      return createBpmPattern(90, durationMs);

    case 'steady-120bpm':
      return createBpmPattern(120, durationMs);

    case 'steady-140bpm':
    case 'fast':
      return createBpmPattern(140, durationMs);

    case 'burst':
    case 'bursts':
      return createBurstPattern(durationMs);

    case 'complex':
      return createComplexPattern(durationMs);

    default:
      // Check for custom BPM format like "120bpm"
      const bpmMatch = type.match(/^(\d+)bpm$/i);
      if (bpmMatch) {
        return createBpmPattern(parseInt(bpmMatch[1], 10), durationMs);
      }

      // Default to 120 BPM
      return createBpmPattern(120, durationMs);
  }
}
