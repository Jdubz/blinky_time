import { afterEach, describe, expect, it, vi } from 'vitest';
import golden from './__fixtures__/golden.json';
import ota from './__fixtures__/ota_vectors.json';
import {
  buildOf,
  fetchReleaseIndex,
  loadOtaRelease,
  loadVerifiedRelease,
  sha256Hex,
  type ReleaseEntry,
} from './releases';

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
      hex: { path: 'b216/blinky-things-b216.hex', sha256: 'f'.repeat(64) },
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
    await expect(fetchReleaseIndex(BASE)).rejects.toThrow(/No firmware releases are published/);
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
  it('downloads and checks only the UF2', async () => {
    const e = await entry();
    const fetchMock = serve({ [BASE + e.files.uf2.path]: { body: uf2Bytes } });
    const rel = await loadVerifiedRelease(e, BASE);
    expect(rel.uf2.length).toBe(uf2Bytes.length);
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });

  it('accepts a UF2 the host labels text/html (Firebase, compressed .uf2)', async () => {
    const e = await entry();
    serve({ [BASE + e.files.uf2.path]: { body: uf2Bytes, type: 'text/html; charset=UTF-8' } });
    await expect(loadVerifiedRelease(e, BASE)).resolves.toBeDefined();
  });

  it('refuses the SPA page served in place of a missing UF2', async () => {
    const e = await entry();
    serve({});
    await expect(loadVerifiedRelease(e, BASE)).rejects.toThrow(/integrity check/);
  });

  it('refuses a file whose sha256 does not match the release manifest', async () => {
    const e = await entry();
    const tampered = uf2Bytes.slice();
    tampered[40] ^= 1;
    serve({ [BASE + e.files.uf2.path]: { body: tampered } });
    await expect(loadVerifiedRelease(e, BASE)).rejects.toThrow(/integrity check/);
  });

  it('refuses a UF2 that passes sha256 but writes outside the application region', async () => {
    const bad = uf2Bytes.slice();
    new DataView(bad.buffer).setUint32(12, 0xf4000, true); // bootloader
    const e = await entry();
    e.files.uf2.sha256 = await sha256Hex(bad);
    serve({ [BASE + e.files.uf2.path]: { body: bad } });
    await expect(loadVerifiedRelease(e, BASE)).rejects.toThrow(/outside the application region/);
  });

  it('refuses a download error', async () => {
    const e = await entry();
    serve({ [BASE + e.files.uf2.path]: { body: '', status: 404 } });
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

describe('loadOtaRelease', () => {
  const image = Uint8Array.from(atob(ota.imageBase64), c => c.charCodeAt(0));
  const manifest = Uint8Array.from(ota.manifestHex.match(/../g)!, b => parseInt(b, 16));

  async function otaEntry(build = ota.build, img = image): Promise<ReleaseEntry> {
    const e = await entry({ tag: `b${build}`, build });
    e.files.bin = { path: `b${build}/blinky-things-b${build}.bin`, sha256: await sha256Hex(img) };
    e.files.otam = {
      path: `b${build}/blinky-things-b${build}.otam`,
      sha256: await sha256Hex(manifest),
    };
    return e;
  }

  it('loads the image and its signed manifest', async () => {
    const e = await otaEntry();
    serve({
      [BASE + e.files.bin!.path]: { body: image },
      [BASE + e.files.otam!.path]: { body: manifest },
    });
    const rel = await loadOtaRelease(e, BASE);
    expect(rel.fields.build).toBe(ota.build);
    expect(rel.image.length).toBe(image.length);
  });

  it('refuses an image that is not the one the manifest hashes', async () => {
    const other = image.slice();
    other[100] ^= 1;
    const e = await otaEntry(ota.build, other); // index sha256 matches the tampered copy
    serve({
      [BASE + e.files.bin!.path]: { body: other },
      [BASE + e.files.otam!.path]: { body: manifest },
    });
    await expect(loadOtaRelease(e, BASE)).rejects.toThrow(/does not match its signed manifest/);
  });

  it('refuses a manifest listed under another release', async () => {
    const e = await otaEntry(ota.build + 1);
    serve({
      [BASE + e.files.bin!.path]: { body: image },
      [BASE + e.files.otam!.path]: { body: manifest },
    });
    await expect(loadOtaRelease(e, BASE)).rejects.toThrow(
      /manifest is for build 215, but the release is b216/
    );
  });

  it('refuses a release published before signed updates', async () => {
    await expect(loadOtaRelease(await entry(), BASE)).rejects.toThrow(/before signed updates/);
  });
});
