/**
 * Type declarations for gifenc
 */
declare module 'gifenc' {
  export function GIFEncoder(): {
    writeFrame(
      index: Uint8Array,
      width: number,
      height: number,
      options?: {
        palette?: number[][];
        delay?: number;
        transparent?: number;
        dispose?: number;
      }
    ): void;
    finish(): void;
    bytes(): Uint8Array;
  };

  export function quantize(
    rgba: Uint8Array | Uint8ClampedArray,
    maxColors: number,
    options?: {
      format?: string;
      oneBitAlpha?: boolean;
      clearAlpha?: boolean;
      clearAlphaThreshold?: number;
      clearAlphaColor?: number;
    }
  ): number[][];

  export function applyPalette(
    rgba: Uint8Array | Uint8ClampedArray,
    palette: number[][],
    format?: string
  ): Uint8Array;

  export function nearestColorIndex(palette: number[][], color: number[]): number;
  export function nearestColorIndexWithDistance(palette: number[][], color: number[]): [number, number];
  export function snapColorsToPalette(palette: number[][], colors: number[][], threshold?: number): void;
  export function prequantize(rgba: Uint8Array, options?: { roundRGB?: number; roundAlpha?: number; oneBitAlpha?: boolean }): void;
}
