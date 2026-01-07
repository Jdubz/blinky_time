/**
 * Simplex Noise implementation for ambient backgrounds
 * Based on Stefan Gustavson's simplex noise algorithm
 */

// Permutation table
const perm = new Uint8Array(512);
const gradP: { x: number; y: number; z: number }[] = new Array(512);

// Gradient vectors for 3D
const grad3 = [
  { x: 1, y: 1, z: 0 }, { x: -1, y: 1, z: 0 }, { x: 1, y: -1, z: 0 }, { x: -1, y: -1, z: 0 },
  { x: 1, y: 0, z: 1 }, { x: -1, y: 0, z: 1 }, { x: 1, y: 0, z: -1 }, { x: -1, y: 0, z: -1 },
  { x: 0, y: 1, z: 1 }, { x: 0, y: -1, z: 1 }, { x: 0, y: 1, z: -1 }, { x: 0, y: -1, z: -1 }
];

// Initialize with seed
function seed(s: number): void {
  const p = new Uint8Array(256);

  // Simple seeded random
  let seed = s;
  const random = () => {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    return seed / 0x7fffffff;
  };

  for (let i = 0; i < 256; i++) {
    p[i] = i;
  }

  // Fisher-Yates shuffle
  for (let i = 255; i > 0; i--) {
    const j = Math.floor(random() * (i + 1));
    [p[i], p[j]] = [p[j], p[i]];
  }

  for (let i = 0; i < 512; i++) {
    perm[i] = p[i & 255];
    gradP[i] = grad3[perm[i] % 12];
  }
}

// Initialize with default seed
seed(42);

const F2 = 0.5 * (Math.sqrt(3) - 1);
const G2 = (3 - Math.sqrt(3)) / 6;

/**
 * 2D Simplex noise
 * Returns value in range [-1, 1]
 */
export function simplex2(x: number, y: number): number {
  let n0 = 0, n1 = 0, n2 = 0;

  // Skew input space
  const s = (x + y) * F2;
  const i = Math.floor(x + s);
  const j = Math.floor(y + s);

  const t = (i + j) * G2;
  const X0 = i - t;
  const Y0 = j - t;
  const x0 = x - X0;
  const y0 = y - Y0;

  // Determine simplex
  let i1: number, j1: number;
  if (x0 > y0) {
    i1 = 1; j1 = 0;
  } else {
    i1 = 0; j1 = 1;
  }

  const x1 = x0 - i1 + G2;
  const y1 = y0 - j1 + G2;
  const x2 = x0 - 1 + 2 * G2;
  const y2 = y0 - 1 + 2 * G2;

  const ii = i & 255;
  const jj = j & 255;

  // Corner contributions
  let t0 = 0.5 - x0 * x0 - y0 * y0;
  if (t0 >= 0) {
    const gi0 = gradP[ii + perm[jj]];
    t0 *= t0;
    n0 = t0 * t0 * (gi0.x * x0 + gi0.y * y0);
  }

  let t1 = 0.5 - x1 * x1 - y1 * y1;
  if (t1 >= 0) {
    const gi1 = gradP[ii + i1 + perm[jj + j1]];
    t1 *= t1;
    n1 = t1 * t1 * (gi1.x * x1 + gi1.y * y1);
  }

  let t2 = 0.5 - x2 * x2 - y2 * y2;
  if (t2 >= 0) {
    const gi2 = gradP[ii + 1 + perm[jj + 1]];
    t2 *= t2;
    n2 = t2 * t2 * (gi2.x * x2 + gi2.y * y2);
  }

  // Scale to [-1, 1]
  return 70 * (n0 + n1 + n2);
}

/**
 * 3D Simplex noise (for time-varying 2D fields)
 * Returns value in range [-1, 1]
 */
export function simplex3(x: number, y: number, z: number): number {
  const F3 = 1 / 3;
  const G3 = 1 / 6;

  const s = (x + y + z) * F3;
  const i = Math.floor(x + s);
  const j = Math.floor(y + s);
  const k = Math.floor(z + s);

  const t = (i + j + k) * G3;
  const X0 = i - t;
  const Y0 = j - t;
  const Z0 = k - t;
  const x0 = x - X0;
  const y0 = y - Y0;
  const z0 = z - Z0;

  let i1: number, j1: number, k1: number;
  let i2: number, j2: number, k2: number;

  if (x0 >= y0) {
    if (y0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
    else if (x0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1; }
    else { i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1; }
  } else {
    if (y0 < z0) { i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1; }
    else if (x0 < z0) { i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1; }
    else { i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
  }

  const x1 = x0 - i1 + G3;
  const y1 = y0 - j1 + G3;
  const z1 = z0 - k1 + G3;
  const x2 = x0 - i2 + 2 * G3;
  const y2 = y0 - j2 + 2 * G3;
  const z2 = z0 - k2 + 2 * G3;
  const x3 = x0 - 1 + 3 * G3;
  const y3 = y0 - 1 + 3 * G3;
  const z3 = z0 - 1 + 3 * G3;

  const ii = i & 255;
  const jj = j & 255;
  const kk = k & 255;

  let n0 = 0, n1 = 0, n2 = 0, n3 = 0;

  let t0 = 0.6 - x0 * x0 - y0 * y0 - z0 * z0;
  if (t0 >= 0) {
    const gi0 = gradP[ii + perm[jj + perm[kk]]];
    t0 *= t0;
    n0 = t0 * t0 * (gi0.x * x0 + gi0.y * y0 + gi0.z * z0);
  }

  let t1 = 0.6 - x1 * x1 - y1 * y1 - z1 * z1;
  if (t1 >= 0) {
    const gi1 = gradP[ii + i1 + perm[jj + j1 + perm[kk + k1]]];
    t1 *= t1;
    n1 = t1 * t1 * (gi1.x * x1 + gi1.y * y1 + gi1.z * z1);
  }

  let t2 = 0.6 - x2 * x2 - y2 * y2 - z2 * z2;
  if (t2 >= 0) {
    const gi2 = gradP[ii + i2 + perm[jj + j2 + perm[kk + k2]]];
    t2 *= t2;
    n2 = t2 * t2 * (gi2.x * x2 + gi2.y * y2 + gi2.z * z2);
  }

  let t3 = 0.6 - x3 * x3 - y3 * y3 - z3 * z3;
  if (t3 >= 0) {
    const gi3 = gradP[ii + 1 + perm[jj + 1 + perm[kk + 1]]];
    t3 *= t3;
    n3 = t3 * t3 * (gi3.x * x3 + gi3.y * y3 + gi3.z * z3);
  }

  return 32 * (n0 + n1 + n2 + n3);
}

/**
 * Fractal Brownian Motion - layered noise for natural patterns
 */
export function fbm(x: number, y: number, z: number, octaves: number = 4, lacunarity: number = 2, gain: number = 0.5): number {
  let value = 0;
  let amplitude = 1;
  let frequency = 1;
  let maxValue = 0;

  for (let i = 0; i < octaves; i++) {
    value += amplitude * simplex3(x * frequency, y * frequency, z * frequency);
    maxValue += amplitude;
    amplitude *= gain;
    frequency *= lacunarity;
  }

  return value / maxValue;
}
