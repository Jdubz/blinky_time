import { afterEach, describe, expect, it, vi } from 'vitest';
import golden from './__fixtures__/golden.json';
import {
  buildOf,
  fetchReleaseIndex,
  loadVerifiedRelease,
  sha256Hex,
  type ReleaseEntry,
} from './releases';

const hexBytes = new TextEncoder().encode(golden.appHex);
const uf2Bytes = Uint8Array.from(atob(golden.appUf2Base64), c => c.charCodeAt(0));
const BASE = 'https://blinky.example/firmware/';

async function entry(overrides: Partial<ReleaseEntry> = {}): Promise<ReleaseEntry> {
  return {
    tag: 'b216',
    build: 216,
    version: 'b216-1a2b3c4d',
    git_sha: '1a2b3c4d5e6f',
    settings_version: 70,
    device_version: 1,
    published_at: '2026-10-10T00:00:00Z',
    url: 'https://github.com/Jdubz/blinky_time/releases/tag/b216',
    files: {
      hex: { path: 'b216/blinky-things-b216.hex', sha256: await sha256Hex(hexBytes) },
      uf2: { path: 'b216/blinky-things-b216.uf2', sha256: await sha256Hex(uf2Bytes) },
    },
    ...overrides,
  };
}

function serve(files: Record<string, { body: BodyInit; type?: string; status?: number }>) {
  const fetchMock = vi.fn(async (url: string) => {
    const f = files[url];
    if (!f)
      return new Response('<!doctype html>', {
        status: 200,
        headers: { 'content-type': 'text/html' },
      });
    return new Response(f.body, {
      status: f.status ?? 200,
      headers: { 'content-type': f.type ?? 'application/octet-stream' },
    });
  });
  vi.stubGlobal('fetch', fetchMock);
  return fetchMock;
}

afterEach(() => vi.unstubAllGlobals());

describe('fetchReleaseIndex', () => {
  it('parses a valid index', async () => {
    const e = await entry();
    serve({
      [BASE + 'index.json']: {
        body: JSON.stringify({ schema: 1, latest: 'b216', releases: [e] }),
        type: 'application/json',
      },
    });
    const idx = await fetchReleaseIndex(BASE);
    expect(idx.latest).toBe('b216');
    expect(idx.releases[0].version).toBe('b216-1a2b3c4d');
  });

  it('fails loudly when the host serves the SPA page instead of the index', async () => {
    serve({});
    await expect(fetchReleaseIndex(BASE)).rejects.toThrow(/not published on this site/);
  });

  it('rejects an index whose latest is not listed', async () => {
    serve({
      [BASE + 'index.json']: {
        body: JSON.stringify({ schema: 1, latest: 'b999', releases: [] }),
        type: 'application/json',
      },
    });
    await expect(fetchReleaseIndex(BASE)).rejects.toThrow(/latest b999 is not in releases/);
  });

  it('rejects a mirror path that escapes the release directory', async () => {
    const e = await entry();
    e.files.uf2.path = '../evil.uf2';
    serve({
      [BASE + 'index.json']: {
        body: JSON.stringify({ schema: 1, latest: 'b216', releases: [e] }),
        type: 'application/json',
      },
    });
    await expect(fetchReleaseIndex(BASE)).rejects.toThrow(/malformed/);
  });
});

describe('loadVerifiedRelease', () => {
  it('verifies and builds both transport images', async () => {
    const e = await entry();
    serve({
      [BASE + e.files.hex.path]: { body: hexBytes },
      [BASE + e.files.uf2.path]: { body: uf2Bytes },
    });
    const rel = await loadVerifiedRelease(e, BASE);
    expect(rel.uf2.length).toBe(uf2Bytes.length);
    expect(rel.dfu.initPacket.length).toBe(14);
  });

  it('refuses a file whose sha256 does not match the release manifest', async () => {
    const e = await entry();
    const tampered = uf2Bytes.slice();
    tampered[40] ^= 1;
    serve({
      [BASE + e.files.hex.path]: { body: hexBytes },
      [BASE + e.files.uf2.path]: { body: tampered },
    });
    await expect(loadVerifiedRelease(e, BASE)).rejects.toThrow(/integrity check/);
  });

  it('refuses a download error', async () => {
    const e = await entry();
    serve({
      [BASE + e.files.hex.path]: { body: hexBytes },
      [BASE + e.files.uf2.path]: { body: '', status: 404 },
    });
    await expect(loadVerifiedRelease(e, BASE)).rejects.toThrow(/HTTP 404/);
  });
});

describe('buildOf', () => {
  it.each([
    ['b216-1a2b3c4d', 216],
    ['b216', 216],
    ['b999-dev-abc', 999],
    ['v1.2', null],
  ])('%s -> %s', (v, n) => expect(buildOf(v)).toBe(n));
});
