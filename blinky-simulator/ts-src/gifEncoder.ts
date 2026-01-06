/**
 * GIF Encoder wrapper
 *
 * Uses gifenc library to create animated GIFs.
 */

import { GIFEncoder, quantize, applyPalette } from 'gifenc';
import * as fs from 'fs';

export class GifWriter {
  private encoder: ReturnType<typeof GIFEncoder>;
  private width: number;
  private height: number;
  private delay: number;  // Frame delay in ms

  constructor(width: number, height: number, fps: number = 30) {
    this.width = width;
    this.height = height;
    this.delay = Math.round(1000 / fps);
    this.encoder = GIFEncoder();
  }

  addFrame(rgba: Uint8Array): void {
    // Quantize to 256 colors
    const palette = quantize(rgba, 256);

    // Apply palette to get indexed frame
    const indexed = applyPalette(rgba, palette);

    // Write frame
    this.encoder.writeFrame(indexed, this.width, this.height, {
      palette,
      delay: this.delay
    });
  }

  finish(): Uint8Array {
    this.encoder.finish();
    return this.encoder.bytes();
  }

  save(filename: string): void {
    const bytes = this.finish();
    fs.writeFileSync(filename, Buffer.from(bytes));
  }
}
