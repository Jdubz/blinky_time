/**
 * Water Generator - Port of blinky-things/generators/Water.cpp
 *
 * Simulates water drops falling and creating splashes.
 * Simplified version that captures the visual essence without full particle system.
 */

import { Generator, AudioControl, PixelMatrix, RGB, createPixelMatrix, AudioControlHelpers } from '../types';

/**
 * Water color palette (matches firmware Palette::WATER)
 * Uses three-segment interpolation: black -> medium blue -> cyan -> light blue
 */
function intensityToColor(intensity: number): RGB {
  // Clamp to 0-255
  intensity = Math.max(0, Math.min(255, Math.floor(intensity * 255)));

  if (intensity === 0) {
    return { r: 0, g: 0, b: 0 };
  } else if (intensity < 85) {
    // Black to medium blue
    const t = intensity / 84;
    return {
      r: 0,
      g: 0,
      b: Math.round(150 * t)
    };
  } else if (intensity < 170) {
    // Medium blue to cyan
    const t = (intensity - 85) / 84;
    return {
      r: 0,
      g: Math.round(120 * t),
      b: Math.round(150 + (255 - 150) * t)
    };
  } else {
    // Cyan to light blue
    const t = (intensity - 170) / 85;
    return {
      r: Math.round(80 * t),
      g: Math.round(120 + (200 - 120) * t),
      b: 255
    };
  }
}

interface Drop {
  x: number;
  y: number;
  vx: number;
  vy: number;
  intensity: number;
  active: boolean;
}

interface Splash {
  x: number;
  y: number;
  vx: number;
  vy: number;
  intensity: number;
  age: number;
  active: boolean;
}

export interface WaterParams {
  baseSpawnChance: number;   // Drop spawn probability (default: 0.05)
  audioSpawnBoost: number;   // Audio boost for spawning (default: 0.4)
  dropVelocityMin: number;   // Minimum drop fall speed (default: 0.5)
  dropVelocityMax: number;   // Maximum drop fall speed (default: 1.5)
  dropSpread: number;        // Horizontal velocity variation (default: 0.3)
  gravity: number;           // Gravity strength (default: 0.3)
  splashParticles: number;   // Particles per splash (default: 4)
  maxParticles: number;      // Maximum concurrent particles (default: 24)
}

export class WaterGenerator implements Generator {
  name = 'Water';

  private width = 0;
  private height = 0;
  private matrix: PixelMatrix | null = null;
  private lastTimeMs = -1;
  private lastPulse = 0;

  private drops: Drop[] = [];
  private splashes: Splash[] = [];

  private params: WaterParams = {
    baseSpawnChance: 0.05,
    audioSpawnBoost: 0.4,
    dropVelocityMin: 0.5,
    dropVelocityMax: 1.5,
    dropSpread: 0.3,
    gravity: 0.3,
    splashParticles: 4,
    maxParticles: 24
  };

  begin(width: number, height: number): void {
    this.width = width;
    this.height = height;
    this.matrix = createPixelMatrix(width, height);
    this.drops = [];
    this.splashes = [];
    this.lastTimeMs = -1;
    this.lastPulse = 0;
  }

  setParams(params: Partial<WaterParams>): void {
    this.params = { ...this.params, ...params };
  }

  private beatHappened(audio: AudioControl): boolean {
    const happened = audio.pulse > 0.5 && this.lastPulse <= 0.5;
    this.lastPulse = audio.pulse;
    return happened;
  }

  private spawnSplash(x: number, y: number, parentIntensity: number): void {
    const { splashParticles, maxParticles } = this.params;
    const totalParticles = this.drops.filter(d => d.active).length + this.splashes.filter(s => s.active).length;

    const available = Math.max(0, maxParticles - totalParticles);
    const count = Math.min(splashParticles, available);

    for (let i = 0; i < count; i++) {
      const angle = (i * Math.PI * 2 / count) + Math.random() * 0.1;
      const speed = 0.3 + Math.random() * 0.3;

      this.splashes.push({
        x,
        y,
        vx: Math.cos(angle) * speed,
        vy: Math.sin(angle) * speed - 0.5,  // Slight upward component
        intensity: parentIntensity * 0.7,
        age: 0,
        active: true
      });
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
    const { baseSpawnChance, audioSpawnBoost, dropVelocityMin, dropVelocityMax, dropSpread, gravity, maxParticles } = this.params;
    const hasRhythm = AudioControlHelpers.hasRhythm(audio);

    // Clear matrix
    this.matrix.clear();

    // 1. Spawn new drops
    let dropCount = 0;
    let spawnProb = baseSpawnChance;

    if (hasRhythm) {
      // Music mode: beat-synced spawning
      if (audio.pulse > 0.3) {
        const beatBoost = AudioControlHelpers.phaseToPulse(audio);
        spawnProb += audioSpawnBoost * audio.pulse * beatBoost;
      }

      if (this.beatHappened(audio)) {
        dropCount = 4;  // Wave on beat
      }
    } else {
      // Organic mode: transient-reactive
      if (audio.pulse > 0.5) {
        spawnProb += audioSpawnBoost * audio.pulse;
        dropCount = 2;
      }
    }

    if (Math.random() < spawnProb) {
      dropCount++;
    }

    // Spawn drops from top
    const totalParticles = this.drops.filter(d => d.active).length + this.splashes.filter(s => s.active).length;
    for (let i = 0; i < dropCount && totalParticles + i < maxParticles; i++) {
      const vy = dropVelocityMin + Math.random() * (dropVelocityMax - dropVelocityMin);
      const vx = (Math.random() - 0.5) * 2 * dropSpread;
      const intensity = 0.6 + Math.random() * 0.4;

      this.drops.push({
        x: Math.random() * this.width,
        y: 0,
        vx,
        vy,
        intensity,
        active: true
      });
    }

    // 2. Update drops
    for (const drop of this.drops) {
      if (!drop.active) continue;

      drop.vy += gravity * dt;
      drop.x += drop.vx * dt;
      drop.y += drop.vy * dt;

      // Splash on bottom collision
      if (drop.y >= this.height - 1) {
        this.spawnSplash(drop.x, this.height - 1, drop.intensity);
        drop.active = false;
      }
    }

    // 3. Update splashes
    for (const splash of this.splashes) {
      if (!splash.active) continue;

      splash.vy += gravity * dt;
      splash.x += splash.vx * dt;
      splash.y += splash.vy * dt;
      splash.age += dt;
      splash.intensity *= 0.95;  // Fade

      // Deactivate when faded or out of bounds
      if (splash.intensity < 0.05 || splash.age > 30 ||
          splash.y >= this.height || splash.y < 0 ||
          splash.x < 0 || splash.x >= this.width) {
        splash.active = false;
      }
    }

    // 4. Render drops
    for (const drop of this.drops) {
      if (!drop.active) continue;

      const x = Math.floor(drop.x);
      const y = Math.floor(drop.y);
      if (x >= 0 && x < this.width && y >= 0 && y < this.height) {
        const color = intensityToColor(drop.intensity);
        const existing = this.matrix.getPixel(x, y);
        // Additive blend
        this.matrix.setPixel(x, y, {
          r: Math.min(255, existing.r + color.r),
          g: Math.min(255, existing.g + color.g),
          b: Math.min(255, existing.b + color.b)
        });
      }
    }

    // 5. Render splashes
    for (const splash of this.splashes) {
      if (!splash.active) continue;

      const x = Math.floor(splash.x);
      const y = Math.floor(splash.y);
      if (x >= 0 && x < this.width && y >= 0 && y < this.height) {
        const color = intensityToColor(splash.intensity);
        const existing = this.matrix.getPixel(x, y);
        this.matrix.setPixel(x, y, {
          r: Math.min(255, existing.r + color.r),
          g: Math.min(255, existing.g + color.g),
          b: Math.min(255, existing.b + color.b)
        });
      }
    }

    // 6. Add base water ambient (subtle wave)
    for (let y = 0; y < this.height; y++) {
      for (let x = 0; x < this.width; x++) {
        const existing = this.matrix.getPixel(x, y);
        if (existing.r === 0 && existing.g === 0 && existing.b === 0) {
          const wave = Math.sin((x + y * 0.5 + timeMs / 500) * 0.8) * 0.05 + 0.08;
          const color = intensityToColor(wave * (1 + audio.energy * 0.3));
          this.matrix.setPixel(x, y, color);
        }
      }
    }

    // Clean up inactive particles
    this.drops = this.drops.filter(d => d.active);
    this.splashes = this.splashes.filter(s => s.active);
  }

  getMatrix(): PixelMatrix {
    if (!this.matrix) {
      throw new Error('Water generator not initialized');
    }
    return this.matrix;
  }

  reset(): void {
    this.drops = [];
    this.splashes = [];
    this.matrix?.clear();
    this.lastTimeMs = -1;
    this.lastPulse = 0;
  }
}
