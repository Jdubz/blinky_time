/**
 * HueRotationEffect - Port of blinky-things/effects/HueRotationEffect.cpp
 *
 * Applies hue rotation to the entire matrix.
 */

import { Effect, PixelMatrix, RGB } from '../types';

function rgbToHsl(r: number, g: number, b: number): [number, number, number] {
  r /= 255;
  g /= 255;
  b /= 255;

  const max = Math.max(r, g, b);
  const min = Math.min(r, g, b);
  let h = 0;
  let s = 0;
  const l = (max + min) / 2;

  if (max !== min) {
    const d = max - min;
    s = l > 0.5 ? d / (2 - max - min) : d / (max + min);

    switch (max) {
      case r:
        h = ((g - b) / d + (g < b ? 6 : 0)) / 6;
        break;
      case g:
        h = ((b - r) / d + 2) / 6;
        break;
      case b:
        h = ((r - g) / d + 4) / 6;
        break;
    }
  }

  return [h, s, l];
}

function hslToRgb(h: number, s: number, l: number): RGB {
  let r: number, g: number, b: number;

  if (s === 0) {
    r = g = b = l;
  } else {
    const hue2rgb = (p: number, q: number, t: number): number => {
      if (t < 0) t += 1;
      if (t > 1) t -= 1;
      if (t < 1 / 6) return p + (q - p) * 6 * t;
      if (t < 1 / 2) return q;
      if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6;
      return p;
    };

    const q = l < 0.5 ? l * (1 + s) : l + s - l * s;
    const p = 2 * l - q;
    r = hue2rgb(p, q, h + 1 / 3);
    g = hue2rgb(p, q, h);
    b = hue2rgb(p, q, h - 1 / 3);
  }

  return {
    r: Math.round(r * 255),
    g: Math.round(g * 255),
    b: Math.round(b * 255)
  };
}

export interface HueRotationParams {
  hueShift: number;  // 0-1, amount to shift hue
}

export class HueRotationEffect implements Effect {
  name = 'HueRotation';

  private hueShift = 0;

  begin(_width: number, _height: number): void {
    // No initialization needed
  }

  setParams(params: Partial<HueRotationParams>): void {
    if (params.hueShift !== undefined) {
      this.hueShift = params.hueShift;
    }
  }

  apply(matrix: PixelMatrix): void {
    if (this.hueShift === 0) return;

    for (let y = 0; y < matrix.height; y++) {
      for (let x = 0; x < matrix.width; x++) {
        const pixel = matrix.getPixel(x, y);

        // Skip black pixels
        if (pixel.r === 0 && pixel.g === 0 && pixel.b === 0) continue;

        // Convert to HSL, shift hue, convert back
        const [h, s, l] = rgbToHsl(pixel.r, pixel.g, pixel.b);
        const newH = (h + this.hueShift) % 1;
        const newColor = hslToRgb(newH, s, l);

        matrix.setPixel(x, y, newColor);
      }
    }
  }

  reset(): void {
    // No state to reset
  }
}
