/**
 * Core types for blinky-simulator
 */

export interface RGB {
  r: number;
  g: number;
  b: number;
}

export interface AudioControl {
  energy: number;      // 0-1, overall audio energy
  pulse: number;       // 0-1, transient/beat strength
  phase: number;       // 0-1, position in beat cycle (0=on-beat, 0.5=off-beat)
  rhythmStrength: number; // 0-1, confidence in rhythm tracking
}

/**
 * Helper functions for AudioControl (matches firmware convenience methods)
 */
export const AudioControlHelpers = {
  /**
   * Is rhythm strong enough to trust phase?
   */
  hasRhythm(audio: AudioControl): boolean {
    return audio.rhythmStrength > 0.5;
  },

  /**
   * Convert phase to pulse intensity.
   * Returns 1.0 at phase=0 (on-beat), 0.0 at phase=0.5 (off-beat).
   */
  phaseToPulse(audio: AudioControl): number {
    return 0.5 + 0.5 * Math.cos(audio.phase * 2 * Math.PI);
  },

  /**
   * Get phase distance from nearest beat.
   * Returns 0.0 when on-beat (phase near 0 or 1), 0.5 when off-beat.
   */
  distanceFromBeat(audio: AudioControl): number {
    return audio.phase < 0.5 ? audio.phase : (1.0 - audio.phase);
  }
};

export interface PixelMatrix {
  width: number;
  height: number;
  pixels: RGB[];

  getPixel(x: number, y: number): RGB;
  setPixel(x: number, y: number, color: RGB): void;
  clear(): void;
}

export function createPixelMatrix(width: number, height: number): PixelMatrix {
  const pixels: RGB[] = new Array(width * height).fill(null).map(() => ({ r: 0, g: 0, b: 0 }));

  return {
    width,
    height,
    pixels,

    getPixel(x: number, y: number): RGB {
      if (x < 0 || x >= width || y < 0 || y >= height) {
        return { r: 0, g: 0, b: 0 };
      }
      return pixels[y * width + x];
    },

    setPixel(x: number, y: number, color: RGB): void {
      if (x >= 0 && x < width && y >= 0 && y < height) {
        pixels[y * width + x] = { ...color };
      }
    },

    clear(): void {
      for (let i = 0; i < pixels.length; i++) {
        pixels[i] = { r: 0, g: 0, b: 0 };
      }
    }
  };
}

export interface Generator {
  name: string;
  begin(width: number, height: number): void;
  update(audio: AudioControl, timeMs: number): void;
  getMatrix(): PixelMatrix;
  reset(): void;
}

export interface Effect {
  name: string;
  begin(width: number, height: number): void;
  apply(matrix: PixelMatrix): void;
  reset(): void;
}

export interface DeviceConfig {
  name: string;
  width: number;
  height: number;
  orientation: 'horizontal' | 'vertical';
}

export const DEVICE_CONFIGS: Record<string, DeviceConfig> = {
  tube: { name: 'TubeLight', width: 4, height: 15, orientation: 'vertical' },
  hat: { name: 'Hat', width: 89, height: 1, orientation: 'horizontal' },
  bucket: { name: 'BucketTotem', width: 16, height: 8, orientation: 'horizontal' }
};
