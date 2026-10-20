import { z } from 'zod';
import { FirmwareError } from './errors';
import { validateUf2 } from './uf2';

/**
 * Stable releases mirrored onto the console's own origin by
 * `tools/firmware_release.py mirror` (run by .github/workflows/
 * blinky-console-deploy.yml). GitHub release assets can't be fetched from a
 * browser (no CORS), so the mirror is the only source.
 */
const Sha256Schema = z.string().regex(/^[0-9a-f]{64}$/, 'sha256 must be 64 lowercase hex chars');
const FileSchema = z.object({
  path: z.string().regex(/^b\d+\/[\w.-]+$/, 'unexpected mirror path'),
  sha256: Sha256Schema,
});

export const ReleaseEntrySchema = z.object({
  tag: z.string().regex(/^b\d+$/),
  build: z.number().int().positive(),
  version: z.string().min(1),
  git_sha: z.string().min(7),
  settings_version: z.number().int(),
  device_version: z.number().int(),
  published_at: z.string(),
  url: z.string().url(),
  files: z.object({ hex: FileSchema, uf2: FileSchema }),
});
export type ReleaseEntry = z.infer<typeof ReleaseEntrySchema>;

export const ReleaseIndexSchema = z
  .object({
    schema: z.literal(1),
    latest: z.string().nullable(),
    releases: z.array(ReleaseEntrySchema),
  })
  .superRefine((idx, ctx) => {
    if (idx.latest !== null && !idx.releases.some(r => r.tag === idx.latest)) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        message: `latest ${idx.latest} is not in releases`,
      });
    }
  });
export type ReleaseIndex = z.infer<typeof ReleaseIndexSchema>;

export function firmwareBaseUrl(): string {
  return new URL('firmware/', new URL(import.meta.env.BASE_URL, window.location.origin)).toString();
}

async function fetchOk(url: string, what: string): Promise<Response> {
  let res: Response;
  try {
    res = await fetch(url, { cache: 'no-store' });
  } catch (err) {
    throw new FirmwareError(`Could not download ${what}: ${String(err)}`);
  }
  if (!res.ok) throw new FirmwareError(`Could not download ${what}: HTTP ${res.status}`);
  return res;
}

export async function fetchReleaseIndex(
  baseUrl: string = firmwareBaseUrl()
): Promise<ReleaseIndex> {
  const res = await fetchOk(new URL('index.json', baseUrl).toString(), 'the firmware release list');
  // The hosting rewrite serves index.html (200) for any missing path. Only
  // the index is judged by its content type: Firebase labels compressed
  // responses for unknown extensions (.uf2, .hex) text/html, so firmware
  // files are judged by their sha256 alone.
  if ((res.headers.get('content-type') ?? '').includes('text/html')) {
    throw new FirmwareError(
      'No firmware releases are published on this site (got an HTML page instead)'
    );
  }
  let raw: unknown;
  try {
    raw = await res.json();
  } catch {
    throw new FirmwareError('The firmware release list is not valid JSON');
  }
  const parsed = ReleaseIndexSchema.safeParse(raw);
  if (!parsed.success) {
    throw new FirmwareError(
      `The firmware release list is malformed: ${parsed.error.issues[0]?.message}`
    );
  }
  return parsed.data;
}

export async function sha256Hex(data: Uint8Array<ArrayBuffer>): Promise<string> {
  const digest = await crypto.subtle.digest('SHA-256', data);
  return Array.from(new Uint8Array(digest), b => b.toString(16).padStart(2, '0')).join('');
}

async function fetchVerified(
  baseUrl: string,
  file: { path: string; sha256: string }
): Promise<Uint8Array<ArrayBuffer>> {
  const res = await fetchOk(new URL(file.path, baseUrl).toString(), file.path);
  const data = new Uint8Array(await res.arrayBuffer());
  const got = await sha256Hex(data);
  if (got !== file.sha256) {
    throw new FirmwareError(
      `${file.path} failed its integrity check (sha256 ${got.slice(0, 12)}…, expected ${file.sha256.slice(0, 12)}…)`
    );
  }
  return data;
}

/** A release whose files passed every check. Only {@link loadVerifiedRelease} makes one. */
export interface VerifiedRelease {
  entry: ReleaseEntry;
  uf2: Uint8Array<ArrayBuffer>;
}

/**
 * Download one release's UF2 and check it before anything touches a device:
 * sha256 against the release manifest, then family / region / numbering of
 * every block. (The mirror also carries the release .hex for the deploy
 * tools; the web flasher only installs the UF2.)
 */
export async function loadVerifiedRelease(
  entry: ReleaseEntry,
  baseUrl: string = firmwareBaseUrl()
): Promise<VerifiedRelease> {
  const uf2 = await fetchVerified(baseUrl, entry.files.uf2);
  validateUf2(uf2);
  return { entry, uf2 };
}

/** Build number of a firmware version string (`b216-1a2b3c4d`, `b216`); null if it has none. */
export function buildOf(version: string): number | null {
  const m = /^b(\d+)(?:-|$)/.exec(version);
  return m ? Number(m[1]) : null;
}
