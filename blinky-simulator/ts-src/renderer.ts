/**
 * LED to Image Renderer
 *
 * Renders PixelMatrix to RGBA image buffer with LED glow effects.
 */

import { PixelMatrix, RGB } from './types';

export interface RendererConfig {
  ledSize: number;      // LED circle diameter in pixels
  ledSpacing: number;   // Space between LEDs
  padding: number;      // Border padding
  drawGlow: boolean;    // Whether to draw glow effect
  bgColor: RGB;         // Background color
}

const DEFAULT_CONFIG: RendererConfig = {
  ledSize: 16,
  ledSpacing: 4,
  padding: 10,
  drawGlow: true,
  bgColor: { r: 16, g: 16, b: 16 }
};

export class LEDRenderer {
  private config: RendererConfig;
  private width = 0;
  private height = 0;
  private buffer: Uint8Array | null = null;

  constructor(config: Partial<RendererConfig> = {}) {
    this.config = { ...DEFAULT_CONFIG, ...config };
  }

  configure(ledWidth: number, ledHeight: number): void {
    const { ledSize, ledSpacing, padding } = this.config;

    this.width = padding * 2 + ledWidth * ledSize + (ledWidth - 1) * ledSpacing;
    this.height = padding * 2 + ledHeight * ledSize + (ledHeight - 1) * ledSpacing;

    // RGBA buffer
    this.buffer = new Uint8Array(this.width * this.height * 4);
  }

  getWidth(): number {
    return this.width;
  }

  getHeight(): number {
    return this.height;
  }

  getBuffer(): Uint8Array {
    if (!this.buffer) {
      throw new Error('Renderer not configured');
    }
    return this.buffer;
  }

  render(matrix: PixelMatrix): void {
    if (!this.buffer) {
      throw new Error('Renderer not configured');
    }

    const { ledSize, ledSpacing, padding, bgColor, drawGlow } = this.config;

    // Clear to background
    for (let i = 0; i < this.buffer.length; i += 4) {
      this.buffer[i] = bgColor.r;
      this.buffer[i + 1] = bgColor.g;
      this.buffer[i + 2] = bgColor.b;
      this.buffer[i + 3] = 255;
    }

    // Draw each LED
    for (let ledY = 0; ledY < matrix.height; ledY++) {
      for (let ledX = 0; ledX < matrix.width; ledX++) {
        const color = matrix.getPixel(ledX, ledY);

        // Calculate center position
        const centerX = padding + ledX * (ledSize + ledSpacing) + ledSize / 2;
        const centerY = padding + ledY * (ledSize + ledSpacing) + ledSize / 2;
        const radius = ledSize / 2;

        // Draw glow (larger, dimmer circle)
        if (drawGlow && (color.r > 0 || color.g > 0 || color.b > 0)) {
          const glowRadius = radius * 1.8;
          for (let py = Math.floor(centerY - glowRadius); py <= Math.ceil(centerY + glowRadius); py++) {
            for (let px = Math.floor(centerX - glowRadius); px <= Math.ceil(centerX + glowRadius); px++) {
              if (px < 0 || px >= this.width || py < 0 || py >= this.height) continue;

              const dx = px - centerX;
              const dy = py - centerY;
              const dist = Math.sqrt(dx * dx + dy * dy);

              if (dist <= glowRadius) {
                const glowIntensity = Math.pow(1 - dist / glowRadius, 2) * 0.3;
                const idx = (py * this.width + px) * 4;

                // Additive blend
                this.buffer[idx] = Math.min(255, this.buffer[idx] + Math.round(color.r * glowIntensity));
                this.buffer[idx + 1] = Math.min(255, this.buffer[idx + 1] + Math.round(color.g * glowIntensity));
                this.buffer[idx + 2] = Math.min(255, this.buffer[idx + 2] + Math.round(color.b * glowIntensity));
              }
            }
          }
        }

        // Draw LED core (solid circle)
        for (let py = Math.floor(centerY - radius); py <= Math.ceil(centerY + radius); py++) {
          for (let px = Math.floor(centerX - radius); px <= Math.ceil(centerX + radius); px++) {
            if (px < 0 || px >= this.width || py < 0 || py >= this.height) continue;

            const dx = px - centerX;
            const dy = py - centerY;
            const dist = Math.sqrt(dx * dx + dy * dy);

            if (dist <= radius) {
              const idx = (py * this.width + px) * 4;

              // Slight edge darkening for 3D effect
              const edgeFactor = 1 - Math.pow(dist / radius, 2) * 0.3;

              this.buffer[idx] = Math.min(255, Math.round(color.r * edgeFactor));
              this.buffer[idx + 1] = Math.min(255, Math.round(color.g * edgeFactor));
              this.buffer[idx + 2] = Math.min(255, Math.round(color.b * edgeFactor));
              this.buffer[idx + 3] = 255;
            }
          }
        }
      }
    }
  }
}
