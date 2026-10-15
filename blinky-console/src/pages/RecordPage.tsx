/**
 * RecordPage — phone-first field recorder. Connects straight to ONE device
 * over Web Bluetooth (no blinky-server), streams raw mic PCM + live analysis
 * into IndexedDB, and exports `.blrec` files for offline analysis
 * (ml-training/scripts/convert_blrec.py). See docs/PHONE_RECORD_MODE_PLAN.md.
 */
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { RecordSession, RecordStats } from '../services/record/RecordSession';
import { buildBlrec } from '../services/record/blrecFormat';
import {
  deleteSession,
  estimateStorage,
  listSessions,
  readChunks,
  SessionMeta,
} from '../services/record/recordStore';
import { SAMPLE_RATE } from '../services/record/recordProtocol';

interface RecordPageProps {
  onClose?: () => void;
}

const ACTIVE_STATES = new Set(['starting', 'recording', 'stopping', 'reconnecting']);

function fmtDuration(ms: number): string {
  const s = Math.floor(ms / 1000);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  const mmss = `${String(m).padStart(2, '0')}:${String(sec).padStart(2, '0')}`;
  return h > 0 ? `${h}:${mmss}` : mmss;
}

function fmtBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  if (n < 1024 * 1024 * 1024) return `${(n / 1024 / 1024).toFixed(1)} MB`;
  return `${(n / 1024 / 1024 / 1024).toFixed(2)} GB`;
}

function Tile({
  label,
  value,
  tone,
}: {
  label: string;
  value: string;
  tone?: 'ok' | 'warn' | 'bad';
}) {
  return (
    <div className={`rec-tile${tone ? ` rec-tile-${tone}` : ''}`}>
      <div className="rec-tile-value">{value}</div>
      <div className="rec-tile-label">{label}</div>
    </div>
  );
}

/** Keep the screen awake while recording — Chrome may throttle a hidden page. */
function useWakeLock(active: boolean): void {
  useEffect(() => {
    if (!active || !('wakeLock' in navigator)) return;
    let lock: WakeLockSentinel | null = null;
    let cancelled = false;
    const acquire = async () => {
      try {
        lock = await navigator.wakeLock.request('screen');
      } catch {
        // Denied (e.g. battery saver). The page still records while visible.
      }
    };
    const onVisible = () => {
      if (document.visibilityState === 'visible' && !cancelled) void acquire();
    };
    void acquire();
    document.addEventListener('visibilitychange', onVisible);
    return () => {
      cancelled = true;
      document.removeEventListener('visibilitychange', onVisible);
      void lock?.release();
    };
  }, [active]);
}

export function RecordPage({ onClose }: RecordPageProps) {
  const sessionRef = useRef<RecordSession | null>(null);
  if (!sessionRef.current) sessionRef.current = new RecordSession();
  const session = sessionRef.current;

  const [stats, setStats] = useState<RecordStats | null>(null);
  const [withMel, setWithMel] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [sessions, setSessions] = useState<SessionMeta[]>([]);
  const [storage, setStorage] = useState<{ usage: number; quota: number } | null>(null);

  const supported = typeof navigator !== 'undefined' && 'bluetooth' in navigator;

  useEffect(() => session.subscribe(setStats), [session]);

  // Leaving the page (Back is disabled mid-recording) releases the device so
  // the fleet server or another phone can connect to it.
  useEffect(
    () => () => {
      if (!ACTIVE_STATES.has(session.state)) void session.disconnect();
    },
    [session]
  );

  const refreshSessions = useCallback(async () => {
    try {
      setSessions(await listSessions());
      setStorage(await estimateStorage());
    } catch (err) {
      setError(`Could not read saved sessions: ${String(err)}`);
    }
  }, []);

  useEffect(() => {
    void refreshSessions();
  }, [refreshSessions]);

  // Refresh the list whenever a recording finishes.
  const state = stats?.state ?? 'idle';
  const prevState = useRef(state);
  useEffect(() => {
    if (ACTIVE_STATES.has(prevState.current) && !ACTIVE_STATES.has(state)) {
      void refreshSessions();
    }
    prevState.current = state;
  }, [state, refreshSessions]);

  useWakeLock(ACTIVE_STATES.has(state));

  // Warn before closing the tab mid-recording.
  useEffect(() => {
    if (!ACTIVE_STATES.has(state)) return;
    const handler = (e: BeforeUnloadEvent) => {
      e.preventDefault();
    };
    window.addEventListener('beforeunload', handler);
    return () => window.removeEventListener('beforeunload', handler);
  }, [state]);

  const run = async (fn: () => Promise<void>) => {
    setBusy(true);
    setError(null);
    try {
      await fn();
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setBusy(false);
    }
  };

  const exportSession = async (meta: SessionMeta) => {
    await run(async () => {
      const chunks = await readChunks(meta.id);
      const blob = buildBlrec(meta, chunks);
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = `${meta.deviceName.replace(/[^\w-]+/g, '_')}_${meta.id}.blrec`;
      a.click();
      setTimeout(() => URL.revokeObjectURL(url), 60_000);
    });
  };

  const removeSession = async (meta: SessionMeta) => {
    if (!window.confirm(`Delete ${meta.id}? This cannot be undone.`)) return;
    await run(async () => {
      await deleteSession(meta.id);
      await refreshSessions();
    });
  };

  const s = stats;
  const st = s?.lastStatus ?? null;
  const integrityLoss = useMemo(() => {
    if (!s) return false;
    return (
      s.audioGapSamples > 0 || s.frameSeqGaps > 0 || s.statusSeqGaps > 0 || s.protocolErrors > 0
    );
  }, [s]);

  const connected = state !== 'idle' && state !== 'connecting';

  return (
    <div className="rec-page">
      <header className="rec-header">
        {onClose && (
          <button className="btn btn-small" onClick={onClose} disabled={ACTIVE_STATES.has(state)}>
            ← Back
          </button>
        )}
        <h1 className="rec-title">Field Recorder</h1>
        <span className={`rec-state rec-state-${state}`}>{state}</span>
      </header>

      {!supported && (
        <div className="shell-warning">
          This browser has no Web Bluetooth. Use Chrome on Android (iOS Safari is not supported).
        </div>
      )}

      <section className="rec-controls">
        <div className="rec-device">{connected ? (s?.deviceName ?? 'Blinky') : 'No device'}</div>
        {!connected ? (
          <button
            className="btn btn-primary rec-big"
            disabled={!supported || busy || state === 'connecting'}
            onClick={() => run(() => session.connect())}
          >
            {state === 'connecting' ? 'Connecting…' : 'Connect device'}
          </button>
        ) : (
          <>
            {state === 'connected' && (
              <>
                <label className="rec-toggle">
                  <input
                    type="checkbox"
                    checked={withMel}
                    onChange={e => setWithMel(e.target.checked)}
                  />
                  Include mel bands (+3 KB/s)
                </label>
                <button
                  className="btn btn-danger rec-big"
                  disabled={busy}
                  onClick={() => run(() => session.start(withMel))}
                >
                  ● Start recording
                </button>
                <button
                  className="btn"
                  disabled={busy}
                  onClick={() => run(() => session.disconnect())}
                >
                  Disconnect
                </button>
              </>
            )}
            {ACTIVE_STATES.has(state) && (
              <button
                className="btn btn-primary rec-big"
                disabled={busy || state === 'stopping'}
                onClick={() => run(() => session.stop())}
              >
                {state === 'stopping' ? 'Stopping…' : '■ Stop'}
              </button>
            )}
          </>
        )}
        {error && <div className="rec-error">{error}</div>}
        {s?.message && <div className="rec-message">{s.message}</div>}
      </section>

      {s && s.sessionId && (
        <section className="rec-tiles">
          <Tile label="Elapsed" value={fmtDuration(s.elapsedMs)} />
          <Tile label="Segment" value={String(Math.max(0, s.segment) + 1)} />
          <Tile label="Data rate" value={`${s.kBps.toFixed(1)} KB/s`} />
          <Tile
            label={s.unsavedBytes > 0 ? 'Saved / UNSAVED' : 'Saved'}
            value={
              s.unsavedBytes > 0
                ? `${fmtBytes(s.bytes)} / ${fmtBytes(s.unsavedBytes)}`
                : fmtBytes(s.bytes)
            }
            tone={s.unsavedBytes > 0 ? 'bad' : undefined}
          />
          <Tile label="Audio received" value={fmtDuration((s.audioSamples / SAMPLE_RATE) * 1000)} />
          {/* Phone-measured loss already includes device-side drops: a device
              ring overrun shows up as a sample-index jump, and a device queue
              drop consumes a sequence number. Adding the device counters on
              top would double-count. */}
          <Tile
            label="Audio gaps (samples)"
            value={String(s.audioGapSamples)}
            tone={s.audioGapSamples > 0 ? 'bad' : 'ok'}
          />
          <Tile
            label="Analysis pkts lost"
            value={String(s.frameSeqGaps + s.statusSeqGaps)}
            tone={s.frameSeqGaps + s.statusSeqGaps > 0 ? 'warn' : 'ok'}
          />
          <Tile
            label="Clipped (this segment)"
            value={String(st?.clipSamples ?? 0)}
            tone={(st?.clipSamples ?? 0) > 0 ? 'warn' : undefined}
          />
          <Tile
            label="Peak (1 s)"
            value={Number.isFinite(s.peakDbfs) ? `${s.peakDbfs.toFixed(1)} dBFS` : '—'}
            tone={s.peakDbfs > -1 ? 'bad' : s.peakDbfs > -6 ? 'warn' : undefined}
          />
          <Tile
            label="Device FPS / max frame"
            value={st ? `${st.fps.toFixed(0)} / ${st.maxFrameMs} ms` : '—'}
          />
          <Tile
            label="FFT overruns"
            value={String(st?.fftOverruns ?? 0)}
            tone={(st?.fftOverruns ?? 0) > 0 ? 'warn' : undefined}
          />
          <Tile label="RSSI / gain" value={st ? `${st.rssi} dBm / ${st.hwGain}` : '—'} />
          <Tile
            label="NN act / level"
            value={
              s.lastFrame ? `${s.lastFrame.nna.toFixed(2)} / ${s.lastFrame.level.toFixed(2)}` : '—'
            }
          />
          <Tile
            label="Protocol errors"
            value={String(s.protocolErrors)}
            tone={s.protocolErrors > 0 ? 'bad' : undefined}
          />
        </section>
      )}
      {integrityLoss && (
        <div className="rec-error">
          Data loss detected — gaps are recorded exactly and the converter will split around them,
          but check signal strength / keep the phone closer.
        </div>
      )}

      <section className="rec-sessions">
        <h2>Saved sessions</h2>
        {storage && (
          <div className="rec-storage">
            Storage: {fmtBytes(storage.usage)} used of {fmtBytes(storage.quota)}
          </div>
        )}
        {sessions.length === 0 && <div className="rec-empty">No recordings yet.</div>}
        {sessions.map(m => (
          <div key={m.id} className="rec-session">
            <div className="rec-session-info">
              <div className="rec-session-name">{m.deviceName}</div>
              <div className="rec-session-meta">
                {new Date(m.createdAt).toLocaleString()} ·{' '}
                {fmtDuration((m.audioSamples / SAMPLE_RATE) * 1000)} · {fmtBytes(m.bytes)} ·{' '}
                {m.segments.length} seg
                {m.audioGapSamples > 0 && ` · ${m.audioGapSamples} gap samples`}
              </div>
            </div>
            <div className="rec-session-actions">
              <button className="btn btn-small" disabled={busy} onClick={() => exportSession(m)}>
                Export
              </button>
              <button
                className="btn btn-small btn-danger"
                disabled={busy || (ACTIVE_STATES.has(state) && m.id === s?.sessionId)}
                onClick={() => removeSession(m)}
              >
                Delete
              </button>
            </div>
          </div>
        ))}
      </section>
    </div>
  );
}
