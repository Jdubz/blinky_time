/**
 * Lightning Generator - Port of blinky-things/generators/Lightning.cpp
 *
 * Creates dramatic lightning bolt effects with branching.
 * Uses Bresenham-like line algorithm for coherent bolt structure.
 */

import { Generator, AudioControl, PixelMatrix, RGB, createPixelMatrix, AudioControlHelpers } from '../types';

/**
 * Lightning color palette (matches firmware Palette::LIGHTNING)
 * Uses three-segment interpolation: black -> bright yellow -> white -> electric blue
 */
function intensityToColor(intensity: number): RGB {
  // Clamp to 0-255
  intensity = Math.max(0, Math.min(255, Math.floor(intensity * 255)));

  if (intensity === 0) {
    return { r: 0, g: 0, b: 0 };
  } else if (intensity < 85) {
    // Black to bright yellow
    const t = intensity / 84;
    return {
      r: Math.round(255 * t),
      g: Math.round(200 * t),
      b: 0
    };
  } else if (intensity < 170) {
    // Bright yellow to white-ish
    const t = (intensity - 85) / 84;
    return {
      r: 255,
      g: Math.round(200 + (255 - 200) * t),
      b: Math.round(180 * t)
    };
  } else {
    // White-ish to electric blue
    const t = (intensity - 170) / 85;
    return {
      r: Math.round(255 - (255 - 150) * t),
      g: Math.round(255 - (255 - 200) * t),
      b: Math.round(180 + (255 - 180) * t)
    };
  }
}

interface BoltSegment {
  x: number;
  y: number;
  intensity: number;
  canBranch: boolean;
}

interface Bolt {
  segments: BoltSegment[];
  age: number;
  active: boolean;
}

export interface LightningParams {
  baseSpawnChance: number;   // Strike probability (default: 0.02)
  audioSpawnBoost: number;   // Audio trigger sensitivity (default: 0.5)
  fadeRate: number;          // Intensity decay per frame (default: 8)
  branchChance: number;      // Branching probability (default: 20 = 20%)
  branchCount: number;       // Branches per spawn point (default: 2)
  maxParticles: number;      // Maximum particles (default: 48)
  defaultLifespan: number;   // Bolt lifespan in frames (default: 15)
}

export class LightningGenerator implements Generator {
  name = 'Lightning';

  private width = 0;
  private height = 0;
  private matrix: PixelMatrix | null = null;
  private lastTimeMs = -1;
  private lastPulse = 0;

  private bolts: Bolt[] = [];

  private params: LightningParams = {
    baseSpawnChance: 0.02,
    audioSpawnBoost: 0.5,
    fadeRate: 8,
    branchChance: 20,
    branchCount: 2,
    maxParticles: 48,
    defaultLifespan: 15
  };

  begin(width: number, height: number): void {
    this.width = width;
    this.height = height;
    this.matrix = createPixelMatrix(width, height);
    this.bolts = [];
    this.lastTimeMs = -1;
    this.lastPulse = 0;
  }

  setParams(params: Partial<LightningParams>): void {
    this.params = { ...this.params, ...params };
  }

  private beatHappened(audio: AudioControl): boolean {
    const happened = audio.pulse > 0.5 && this.lastPulse <= 0.5;
    this.lastPulse = audio.pulse;
    return happened;
  }

  private getTotalSegments(): number {
    return this.bolts.reduce((sum, b) => sum + (b.active ? b.segments.length : 0), 0);
  }

  /**
   * Spawn a coherent lightning bolt as a connected chain of segments
   * Uses line algorithm similar to firmware's Bresenham approach
   */
  private spawnBolt(intensity: number): void {
    const { maxParticles } = this.params;

    // Random start and end points
    const x0 = Math.random() * this.width;
    const y0 = Math.random() * this.height;
    const x1 = Math.random() * this.width;
    const y1 = Math.random() * this.height;

    // Calculate steps
    const dx = Math.abs(x1 - x0);
    const dy = Math.abs(y1 - y0);
    let steps = Math.max(dx, dy);
    steps = Math.min(steps, 12);  // Max 12 particles per bolt

    if (steps < 2) return;

    const segments: BoltSegment[] = [];
    const xStep = (x1 - x0) / steps;
    const yStep = (y1 - y0) / steps;

    for (let i = 0; i <= steps && this.getTotalSegments() + segments.length < maxParticles; i++) {
      let x = x0 + xStep * i;
      let y = y0 + yStep * i;

      // Add small random jitter for organic look
      x += (Math.random() - 0.5) * 0.6;
      y += (Math.random() - 0.5) * 0.6;

      segments.push({
        x,
        y,
        intensity,
        canBranch: i > 2 && i < steps - 2  // Can branch in middle section
      });
    }

    if (segments.length > 0) {
      this.bolts.push({
        segments,
        age: 0,
        active: true
      });
    }
  }

  /**
   * Spawn branches from a parent segment
   */
  private spawnBranch(parentX: number, parentY: number, parentIntensity: number): void {
    const { branchCount, maxParticles } = this.params;

    for (let b = 0; b < branchCount && this.getTotalSegments() < maxParticles; b++) {
      const branchLength = 3 + Math.floor(Math.random() * 3);
      const angle = Math.random() * Math.PI * 2;

      const segments: BoltSegment[] = [];
      let intensity = parentIntensity * 0.6;  // Branches are dimmer

      for (let i = 0; i < branchLength && this.getTotalSegments() + segments.length < maxParticles; i++) {
        const t = i / branchLength;
        const x = parentX + Math.cos(angle) * t * branchLength + (Math.random() - 0.5) * 0.4;
        const y = parentY + Math.sin(angle) * t * branchLength + (Math.random() - 0.5) * 0.4;

        segments.push({
          x,
          y,
          intensity: intensity * (1 - t * 0.5),  // Fade along branch
          canBranch: false
        });
      }

      if (segments.length > 0) {
        this.bolts.push({
          segments,
          age: 0,
          active: true
        });
      }
    }
  }

  update(audio: AudioControl, timeMs: number): void {
    if (!this.matrix) return;

    // Handle first frame
    if (this.lastTimeMs < 0) {
      this.lastTimeMs = timeMs;
    }

    const deltaMs = Math.min(timeMs - this.lastTimeMs, 100);
    this.lastTimeMs = timeMs;

    if (deltaMs < 16) return;

    const dt = deltaMs / 33;  // Normalize to ~30fps
    const { baseSpawnChance, audioSpawnBoost, fadeRate, branchChance, maxParticles } = this.params;
    const hasRhythm = AudioControlHelpers.hasRhythm(audio);

    // Clear matrix
    this.matrix.clear();

    // 1. Spawn new bolts
    let boltCount = 0;
    let spawnProb = baseSpawnChance;

    if (hasRhythm) {
      // Music mode: beat-synced spawning
      if (audio.pulse > 0.3) {
        const beatBoost = AudioControlHelpers.phaseToPulse(audio);
        spawnProb += audioSpawnBoost * audio.pulse * beatBoost;
      }

      if (this.beatHappened(audio)) {
        boltCount = 2;
      }
    } else {
      // Organic mode: transient-reactive
      if (audio.pulse > 0.5) {
        spawnProb += audioSpawnBoost * audio.pulse;
        boltCount = 1;
      }
    }

    if (Math.random() < spawnProb) {
      boltCount++;
    }

    // Calculate bolt intensity based on audio
    let intensity = 0.6 + Math.random() * 0.4;
    if (hasRhythm) {
      const phaseMod = AudioControlHelpers.phaseToPulse(audio);
      intensity *= 0.6 + 0.4 * phaseMod;
    }

    for (let i = 0; i < boltCount && this.getTotalSegments() < maxParticles; i++) {
      this.spawnBolt(intensity);
    }

    // 2. Update bolts
    for (const bolt of this.bolts) {
      if (!bolt.active) continue;

      bolt.age += dt;

      // Process segments
      for (const seg of bolt.segments) {
        // Check for branching (young segments only)
        if (seg.canBranch && bolt.age > 2 && bolt.age < 8) {
          if (Math.random() * 100 < branchChance && this.getTotalSegments() < maxParticles) {
            this.spawnBranch(seg.x, seg.y, seg.intensity);
            seg.canBranch = false;  // Only branch once
          }
        }

        // Fast fade
        seg.intensity = Math.max(0, seg.intensity - fadeRate * dt / 255);
      }

      // Deactivate if all faded
      if (bolt.segments.every(s => s.intensity <= 0)) {
        bolt.active = false;
      }
    }

    // 3. Render bolts with MAX blending (brightest wins)
    for (const bolt of this.bolts) {
      if (!bolt.active) continue;

      for (const seg of bolt.segments) {
        if (seg.intensity <= 0) continue;

        const x = Math.floor(seg.x);
        const y = Math.floor(seg.y);

        if (x >= 0 && x < this.width && y >= 0 && y < this.height) {
          const color = intensityToColor(seg.intensity);
          const existing = this.matrix.getPixel(x, y);

          // MAX blending: take brightest value
          this.matrix.setPixel(x, y, {
            r: Math.max(existing.r, color.r),
            g: Math.max(existing.g, color.g),
            b: Math.max(existing.b, color.b)
          });
        }
      }
    }

    // Clean up inactive bolts
    this.bolts = this.bolts.filter(b => b.active);
  }

  getMatrix(): PixelMatrix {
    if (!this.matrix) {
      throw new Error('Lightning generator not initialized');
    }
    return this.matrix;
  }

  reset(): void {
    this.bolts = [];
    this.matrix?.clear();
    this.lastTimeMs = -1;
    this.lastPulse = 0;
  }
}
