/**
 * Fire Generator - Port of blinky-things/generators/Fire.cpp
 *
 * Uses heat diffusion simulation with sparks to create realistic fire effect.
 * Simplified version that captures the visual essence without full particle system.
 */

import { Generator, AudioControl, PixelMatrix, RGB, createPixelMatrix, AudioControlHelpers } from '../types';

/**
 * Fire color palette (matches firmware Fire::particleColor)
 * Uses three-segment interpolation: black -> red -> orange -> yellow
 */
function heatToColor(heat: number): RGB {
  // Clamp to 0-255
  heat = Math.max(0, Math.min(255, Math.floor(heat)));

  if (heat === 0) {
    return { r: 0, g: 0, b: 0 };
  } else if (heat < 85) {
    // Black to red (intensity * 3, capped at 255)
    const red = Math.min(255, heat * 3);
    return { r: red, g: 0, b: 0 };
  } else if (heat < 170) {
    // Red to orange (add green)
    const green = Math.min(255, (heat - 85) * 3);
    return { r: 255, g: green, b: 0 };
  } else {
    // Orange to yellow
    return { r: 255, g: 255, b: 0 };
  }
}

export interface FireParams {
  baseCooling: number;       // Base cooling rate per frame (default: 40)
  sparkChance: number;       // Spark spawn probability per cell (default: 0.08)
  sparkHeatMin: number;      // Minimum spark heat (default: 80)
  sparkHeatMax: number;      // Maximum spark heat (default: 180)
  audioSpawnBoost: number;   // Audio boost multiplier (default: 0.6)
  bottomRowsForSparks: number; // Rows for spark spawning (default: 2)
  trailHeatFactor: number;   // Heat trail intensity (default: 35)
  burstSparks: number;       // Sparks per audio burst (default: 8)
}

export class FireGenerator implements Generator {
  name = 'Fire';

  private width = 0;
  private height = 0;
  private heatMap: number[] = [];
  private matrix: PixelMatrix | null = null;
  private lastTimeMs = -1;  // -1 to handle first frame properly
  private beatCount = 0;
  private lastPulse = 0;

  private params: FireParams = {
    baseCooling: 40,
    sparkChance: 0.08,
    sparkHeatMin: 80,
    sparkHeatMax: 180,
    audioSpawnBoost: 0.6,
    bottomRowsForSparks: 2,
    trailHeatFactor: 35,
    burstSparks: 8
  };

  begin(width: number, height: number): void {
    this.width = width;
    this.height = height;
    this.heatMap = new Array(width * height).fill(0);
    this.matrix = createPixelMatrix(width, height);
    this.lastTimeMs = -1;
    this.beatCount = 0;
    this.lastPulse = 0;
  }

  setParams(params: Partial<FireParams>): void {
    this.params = { ...this.params, ...params };
  }

  private beatHappened(audio: AudioControl): boolean {
    // Detect rising edge of pulse above threshold
    const happened = audio.pulse > 0.5 && this.lastPulse <= 0.5;
    this.lastPulse = audio.pulse;
    return happened;
  }

  update(audio: AudioControl, timeMs: number): void {
    if (!this.matrix) return;

    // Handle first frame properly
    if (this.lastTimeMs < 0) {
      this.lastTimeMs = timeMs;
    }

    // Calculate delta time (cap at 100ms for stability)
    const deltaMs = Math.min(timeMs - this.lastTimeMs, 100);
    this.lastTimeMs = timeMs;

    // Minimum 16ms between updates (60fps max)
    if (deltaMs < 16) return;

    const { baseCooling, sparkChance, sparkHeatMin, sparkHeatMax, audioSpawnBoost, bottomRowsForSparks, burstSparks } = this.params;
    const hasRhythm = AudioControlHelpers.hasRhythm(audio);

    // 1. Apply cooling to heat buffer
    const coolingVariation = hasRhythm
      ? Math.cos(audio.phase * Math.PI * 2) * 15  // Breathing effect
      : 0;

    for (let i = 0; i < this.heatMap.length; i++) {
      const coolAmount = Math.floor(Math.random() * (baseCooling + coolingVariation + 1));
      this.heatMap[i] = Math.max(0, this.heatMap[i] - coolAmount * (deltaMs / 33));
    }

    // 2. Heat diffusion (propagate upward with neighbor averaging)
    for (let y = 0; y < this.height - 2; y++) {
      for (let x = 0; x < this.width; x++) {
        const currentIdx = y * this.width + x;
        const belowIdx = (y + 1) * this.width + x;
        const below2Idx = (y + 2) * this.width + x;

        // Accumulate heat from cells below
        let totalHeat = this.heatMap[belowIdx] + this.heatMap[below2Idx] * 2;
        let divisor = 3;

        // Add horizontal spread
        if (x > 0) {
          totalHeat += this.heatMap[belowIdx - 1];
          divisor++;
        }
        if (x < this.width - 1) {
          totalHeat += this.heatMap[belowIdx + 1];
          divisor++;
        }

        this.heatMap[currentIdx] = Math.min(255, totalHeat / divisor);
      }
    }

    // 3. Spawn sparks based on audio
    let sparkCount = 0;
    let spawnProb = sparkChance;

    if (hasRhythm) {
      // Music mode: beat-synced spawning
      const phaseMod = AudioControlHelpers.phaseToPulse(audio);
      spawnProb += audioSpawnBoost * audio.pulse * phaseMod;

      if (this.beatHappened(audio)) {
        this.beatCount++;
        // Stronger bursts on downbeats (every 4 beats)
        const baseSparks = (this.beatCount % 4 === 0) ? burstSparks * 2 : burstSparks;
        sparkCount = Math.floor(baseSparks * (0.5 + 0.5 * audio.rhythmStrength));
      }
    } else {
      // Organic mode: transient-reactive
      if (audio.pulse > 0.5) {
        spawnProb += audioSpawnBoost * audio.pulse;
        sparkCount = Math.floor(burstSparks / 2);
      }
    }

    // Random baseline spawning
    if (Math.random() < spawnProb) {
      sparkCount++;
    }

    // Spawn sparks at bottom
    for (let i = 0; i < sparkCount; i++) {
      const x = Math.floor(Math.random() * this.width);
      for (let row = 0; row < bottomRowsForSparks; row++) {
        const y = this.height - 1 - row;
        const idx = y * this.width + x;
        const heat = sparkHeatMin + Math.random() * (sparkHeatMax - sparkHeatMin);
        this.heatMap[idx] = Math.min(255, this.heatMap[idx] + heat);
      }
    }

    // 4. Convert heat map to colors
    for (let y = 0; y < this.height; y++) {
      for (let x = 0; x < this.width; x++) {
        const heat = this.heatMap[y * this.width + x];
        const color = heatToColor(heat);
        this.matrix.setPixel(x, y, color);
      }
    }
  }

  getMatrix(): PixelMatrix {
    if (!this.matrix) {
      throw new Error('Fire generator not initialized');
    }
    return this.matrix;
  }

  reset(): void {
    this.heatMap.fill(0);
    this.matrix?.clear();
    this.lastTimeMs = -1;
    this.beatCount = 0;
    this.lastPulse = 0;
  }
}
