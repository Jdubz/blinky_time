/**
 * IndexedDB persistence for record sessions.
 *
 * A session is written incrementally as ~1 s chunks so a tab crash, phone
 * reboot, or battery death loses at most the last chunk. Export assembles
 * the chunks into a single `.blrec` file (see buildBlrec / blrecFormat).
 */

const DB_NAME = 'blinky-record';
const DB_VERSION = 1;
const SESSIONS = 'sessions';
const CHUNKS = 'chunks';

export interface SegmentMeta {
  index: number;
  /** Phone wall clock (ms since epoch) when the segment's start reply arrived. */
  startedAt: number;
  endedAt?: number;
  /** Raw JSON reply to `record start` (device build, gain, MTU, startIdx, ...). */
  startReply: Record<string, unknown>;
  endReason?: string;
}

export interface SessionMeta {
  id: string;
  createdAt: number;
  updatedAt: number;
  deviceName: string;
  withMel: boolean;
  segments: SegmentMeta[];
  chunkCount: number;
  bytes: number;
  audioSamples: number;
  audioGapSamples: number;
  frameSeqGaps: number;
  protocolErrors: number;
  userAgent: string;
}

function req<T>(r: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
}

function txDone(tx: IDBTransaction): Promise<void> {
  return new Promise((resolve, reject) => {
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
    tx.onabort = () => reject(tx.error ?? new Error('IndexedDB transaction aborted'));
  });
}

let dbPromise: Promise<IDBDatabase> | null = null;

function openDb(): Promise<IDBDatabase> {
  if (!dbPromise) {
    dbPromise = new Promise((resolve, reject) => {
      const r = indexedDB.open(DB_NAME, DB_VERSION);
      r.onupgradeneeded = () => {
        const db = r.result;
        if (!db.objectStoreNames.contains(SESSIONS)) {
          db.createObjectStore(SESSIONS, { keyPath: 'id' });
        }
        if (!db.objectStoreNames.contains(CHUNKS)) {
          db.createObjectStore(CHUNKS, { keyPath: ['sessionId', 'idx'] });
        }
      };
      r.onsuccess = () => resolve(r.result);
      r.onerror = () => {
        dbPromise = null;
        reject(r.error);
      };
    });
  }
  return dbPromise;
}

export async function putSession(meta: SessionMeta): Promise<void> {
  const db = await openDb();
  const tx = db.transaction(SESSIONS, 'readwrite');
  tx.objectStore(SESSIONS).put(meta);
  await txDone(tx);
}

/**
 * Append one chunk and update the session meta in one transaction. `meta`
 * (the caller's live object) is only advanced after the commit succeeds, so
 * a failed write leaves chunk indices and byte counts consistent for retry.
 */
export async function appendChunk(meta: SessionMeta, data: ArrayBuffer): Promise<void> {
  const db = await openDb();
  const next: SessionMeta = {
    ...meta,
    chunkCount: meta.chunkCount + 1,
    bytes: meta.bytes + data.byteLength,
    updatedAt: Date.now(),
  };
  const tx = db.transaction([SESSIONS, CHUNKS], 'readwrite');
  tx.objectStore(CHUNKS).put({ sessionId: meta.id, idx: meta.chunkCount, data });
  tx.objectStore(SESSIONS).put(next);
  await txDone(tx);
  meta.chunkCount = next.chunkCount;
  meta.bytes = next.bytes;
  meta.updatedAt = next.updatedAt;
}

export async function listSessions(): Promise<SessionMeta[]> {
  const db = await openDb();
  const all = await req(db.transaction(SESSIONS).objectStore(SESSIONS).getAll());
  return (all as SessionMeta[]).sort((a, b) => b.createdAt - a.createdAt);
}

export async function readChunks(sessionId: string): Promise<ArrayBuffer[]> {
  const db = await openDb();
  const range = IDBKeyRange.bound([sessionId, 0], [sessionId, Number.MAX_SAFE_INTEGER]);
  const rows = await req(db.transaction(CHUNKS).objectStore(CHUNKS).getAll(range));
  return (rows as { idx: number; data: ArrayBuffer }[])
    .sort((a, b) => a.idx - b.idx)
    .map(r => r.data);
}

export async function deleteSession(sessionId: string): Promise<void> {
  const db = await openDb();
  const tx = db.transaction([SESSIONS, CHUNKS], 'readwrite');
  tx.objectStore(SESSIONS).delete(sessionId);
  tx.objectStore(CHUNKS).delete(
    IDBKeyRange.bound([sessionId, 0], [sessionId, Number.MAX_SAFE_INTEGER])
  );
  await txDone(tx);
}

export async function estimateStorage(): Promise<{ usage: number; quota: number } | null> {
  if (!navigator.storage?.estimate) return null;
  const e = await navigator.storage.estimate();
  return { usage: e.usage ?? 0, quota: e.quota ?? 0 };
}

/** Ask the browser not to evict our data under storage pressure. */
export async function requestPersistence(): Promise<boolean> {
  if (!navigator.storage?.persist) return false;
  return navigator.storage.persist();
}
