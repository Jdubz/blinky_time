/**
 * FirmwarePage (#firmware) — update ONE device to a stable release, straight
 * from the browser over USB or Bluetooth (no blinky-server). Releases come
 * from the stable-release mirror the console deploy publishes; every check
 * lives in services/firmware. See docs/WEB_FLASHER.md.
 */
import { useCallback, useEffect, useRef, useState } from 'react';
import { useWakeLock } from '../hooks/useWakeLock';
import { bleFlashSupported, flashOverBle, recoverOverBle } from '../services/firmware/bleFlash';
import type { Confirmation, FlashInfo } from '../services/firmware/deviceGates';
import { FirmwareError } from '../services/firmware/errors';
import type { FlashUi } from '../services/firmware/flashFlow';
import {
  fetchReleaseIndex,
  loadVerifiedRelease,
  type ReleaseIndex,
  type VerifiedRelease,
} from '../services/firmware/releases';
import { flashOverUsb, recoverOverUsb, usbFlashSupported } from '../services/firmware/usbFlash';

type Method = 'usb' | 'ble';
type Mode = 'update' | 'recover';

interface LogLine {
  t: number;
  level: 'info' | 'warn' | 'error';
  message: string;
}

interface PendingAsk {
  prompt: string;
  options: { label: string; run: () => void }[];
}

interface PendingConfirm {
  items: Confirmation[];
  resolve: (ok: boolean) => void;
}

type Outcome = { ok: true; info: FlashInfo } | { ok: false; error: string } | null;

function fmtDate(iso: string): string {
  const d = new Date(iso);
  return Number.isNaN(d.getTime()) ? iso : d.toLocaleDateString();
}

interface FirmwarePageProps {
  onClose?: () => void;
  /**
   * Reports whether an update is running, so the shell keeps this page
   * mounted through hash navigation (Back) until it finishes.
   */
  onActiveChange?: (active: boolean) => void;
}

export function FirmwarePage({ onClose, onActiveChange }: FirmwarePageProps) {
  const [index, setIndex] = useState<ReleaseIndex | null>(null);
  const [indexError, setIndexError] = useState<string | null>(null);
  const [tag, setTag] = useState<string>('');
  const usbOk = usbFlashSupported();
  const bleOk = bleFlashSupported();
  const [method, setMethod] = useState<Method>(usbOk ? 'usb' : 'ble');
  const [mode, setMode] = useState<Mode>('update');

  const [running, setRunning] = useState(false);
  const [committed, setCommitted] = useState(false);
  const [progress, setProgress] = useState<{ label: string; fraction: number | null } | null>(null);
  const [log, setLog] = useState<LogLine[]>([]);
  const [ask, setAsk] = useState<PendingAsk | null>(null);
  const [confirm, setConfirm] = useState<PendingConfirm | null>(null);
  const [accepted, setAccepted] = useState<Set<number>>(new Set());
  const [outcome, setOutcome] = useState<Outcome>(null);
  const abortRef = useRef<AbortController | null>(null);

  useWakeLock(running);
  useEffect(() => onActiveChange?.(running), [running, onActiveChange]);

  useEffect(() => {
    fetchReleaseIndex()
      .then(idx => {
        setIndex(idx);
        if (idx.latest) setTag(idx.latest);
      })
      .catch(err => setIndexError(err instanceof Error ? err.message : String(err)));
  }, []);

  // Once the device is being written, leaving the page must take a decision.
  useEffect(() => {
    if (!committed) return;
    const onUnload = (e: BeforeUnloadEvent) => {
      e.preventDefault();
      e.returnValue = '';
    };
    window.addEventListener('beforeunload', onUnload);
    return () => window.removeEventListener('beforeunload', onUnload);
  }, [committed]);

  const addLog = useCallback((message: string, level: LogLine['level'] = 'info') => {
    setLog(l => [...l, { t: Date.now(), level, message }]);
  }, []);

  const start = async () => {
    if (!index) return;
    const entry = index.releases.find(r => r.tag === tag);
    if (!entry) return;
    const abort = new AbortController();
    abortRef.current = abort;
    setRunning(true);
    setCommitted(false);
    setOutcome(null);
    setLog([]);

    const ui: FlashUi = {
      signal: abort.signal,
      log: addLog,
      progress: (label, fraction) => setProgress({ label, fraction }),
      committed: () => setCommitted(true),
      confirm: items =>
        new Promise<boolean>(resolve => {
          const finish = (ok: boolean) => {
            abort.signal.removeEventListener('abort', onAbort);
            setConfirm(null);
            resolve(ok);
          };
          const onAbort = () => finish(false);
          abort.signal.addEventListener('abort', onAbort, { once: true });
          setAccepted(new Set());
          setConfirm({ items, resolve: finish });
        }),
      ask: <T,>(prompt: string, options: { label: string; action: () => Promise<T> }[]) =>
        new Promise<T>((resolve, reject) => {
          if (abort.signal.aborted) return reject(new FirmwareError('Cancelled'));
          let settled = false;
          const settle = (fn: () => void) => {
            if (settled) return;
            settled = true;
            abort.signal.removeEventListener('abort', onAbort);
            fn();
          };
          // Cancel stays live while an option's action runs (a picker, a
          // connect). The action may still finish in the background, so each
          // one re-checks the signal right before anything irreversible.
          const onAbort = () => {
            setAsk(null);
            settle(() => reject(new FirmwareError('Cancelled')));
          };
          abort.signal.addEventListener('abort', onAbort, { once: true });
          setProgress(null);
          setAsk({
            prompt,
            options: options.map(o => ({
              label: o.label,
              // Runs inside the click: device pickers need the user gesture.
              run: () => {
                setAsk(null);
                o.action().then(
                  v => settle(() => resolve(v)),
                  e => settle(() => reject(e))
                );
              },
            })),
          });
        }),
    };

    try {
      ui.progress(`Downloading and checking ${entry.tag}`, null);
      let release: VerifiedRelease;
      try {
        release = await loadVerifiedRelease(entry);
      } catch (err) {
        throw new FirmwareError(
          `The firmware files did not pass their checks, so nothing was sent to the device: ${
            err instanceof Error ? err.message : String(err)
          }`
        );
      }
      addLog(`${entry.version}: checksums, image layout and Bluetooth package verified`);
      // Cancel pressed during the download: stop before any device interaction.
      if (abort.signal.aborted) throw new FirmwareError('Cancelled');
      const info =
        mode === 'recover'
          ? method === 'usb'
            ? await recoverOverUsb(release, ui)
            : await recoverOverBle(release, ui)
          : method === 'usb'
            ? await flashOverUsb(release, index, ui)
            : await flashOverBle(release, index, ui);
      addLog(`Done: ${info.device.name ?? info.device.id} is running ${release.entry.version}`);
      setOutcome({ ok: true, info });
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err);
      addLog(message, 'error');
      setOutcome({ ok: false, error: message });
    } finally {
      setRunning(false);
      setCommitted(false);
      setProgress(null);
      setAsk(null);
      setConfirm(null);
      abortRef.current = null;
    }
  };

  const copyLog = () => {
    const text = log
      .map(l => `${new Date(l.t).toISOString()} ${l.level.toUpperCase()} ${l.message}`)
      .join('\n');
    void navigator.clipboard?.writeText(text);
  };

  const selected = index?.releases.find(r => r.tag === tag) ?? null;
  const supported = method === 'usb' ? usbOk : bleOk;

  return (
    <div className="rec-page fw-page">
      <header className="rec-header">
        {onClose && (
          <button className="btn btn-small" onClick={onClose} disabled={running}>
            ← Back
          </button>
        )}
        <h1 className="rec-title">Firmware Update</h1>
      </header>

      {!usbOk && !bleOk && (
        <div className="shell-warning">
          This browser can't talk to devices. Use Chrome or Edge on a computer (USB or Bluetooth),
          or Chrome on Android (Bluetooth). iPhone and iPad browsers are not supported.
        </div>
      )}
      {indexError && (
        <div className="shell-warning">Could not load the release list: {indexError}</div>
      )}
      {index && !index.latest && (
        <div className="shell-warning">No stable firmware release has been published yet.</div>
      )}

      {index?.latest && (
        <section className="fw-setup">
          <label className="fw-field">
            <span>Release</span>
            <select value={tag} onChange={e => setTag(e.target.value)} disabled={running}>
              {index.releases.map(r => (
                <option key={r.tag} value={r.tag}>
                  {r.tag}
                  {r.tag === index.latest ? ' — latest stable' : ''} ({fmtDate(r.published_at)})
                </option>
              ))}
            </select>
          </label>
          {selected && (
            <a className="fw-notes" href={selected.url} target="_blank" rel="noreferrer">
              Release notes for {selected.tag}
            </a>
          )}

          <div className="fw-field" role="radiogroup" aria-label="Connection">
            <span>Connection</span>
            <div className="fw-choices">
              <label>
                <input
                  type="radio"
                  checked={method === 'usb'}
                  disabled={running || !usbOk}
                  onChange={() => setMethod('usb')}
                />
                USB cable{!usbOk && ' (Chrome or Edge on a computer)'}
              </label>
              <label>
                <input
                  type="radio"
                  checked={method === 'ble'}
                  disabled={running || !bleOk}
                  onChange={() => setMethod('ble')}
                />
                Bluetooth{!bleOk && ' (not available in this browser)'}
              </label>
            </div>
          </div>

          <label className="rec-toggle">
            <input
              type="checkbox"
              checked={mode === 'recover'}
              disabled={running}
              onChange={e => setMode(e.target.checked ? 'recover' : 'update')}
            />
            Device stuck in update mode (USB drive XIAO-SENSE, or Bluetooth name AdaDFU)
          </label>

          {!running && (
            <button
              className="btn btn-primary rec-big"
              disabled={!supported || !selected}
              onClick={() => void start()}
            >
              {mode === 'recover' ? `Install ${tag}` : `Update to ${tag}`}
            </button>
          )}
          {running && !committed && (
            <button className="btn" onClick={() => abortRef.current?.abort()}>
              Cancel
            </button>
          )}
        </section>
      )}

      {committed && running && (
        <div className="shell-warning">
          Updating — keep this page open and the device powered until it finishes.
        </div>
      )}

      {progress && (
        <div className="fw-progress">
          <div>{progress.label}</div>
          {progress.fraction !== null && <progress max={1} value={progress.fraction} />}
        </div>
      )}

      {confirm && (
        <section className="fw-confirm">
          <h2>Before updating</h2>
          {confirm.items.map((c, i) => (
            <label key={i} className="rec-toggle">
              <input
                type="checkbox"
                checked={accepted.has(i)}
                onChange={e =>
                  setAccepted(s => {
                    const n = new Set(s);
                    if (e.target.checked) n.add(i);
                    else n.delete(i);
                    return n;
                  })
                }
              />
              {c.message}
            </label>
          ))}
          <div className="fw-actions">
            <button
              className="btn btn-primary"
              disabled={accepted.size !== confirm.items.length}
              onClick={() => confirm.resolve(true)}
            >
              Continue
            </button>
            <button className="btn" onClick={() => confirm.resolve(false)}>
              Cancel
            </button>
          </div>
        </section>
      )}

      {ask && (
        <section className="fw-ask">
          <p>{ask.prompt}</p>
          <div className="fw-actions">
            {ask.options.map((o, i) => (
              <button
                key={o.label}
                className={`btn${i === 0 ? ' btn-primary rec-big' : ''}`}
                onClick={o.run}
              >
                {o.label}
              </button>
            ))}
          </div>
        </section>
      )}

      {outcome?.ok && (
        <div className="fw-result fw-result-ok">
          Updated: {outcome.info.device.name ?? outcome.info.device.id} is running{' '}
          {outcome.info.version}.
        </div>
      )}
      {outcome && !outcome.ok && <div className="fw-result fw-result-bad">{outcome.error}</div>}

      {log.length > 0 && (
        <section className="fw-log">
          <div className="fw-log-head">
            <h2>Log</h2>
            <button className="btn btn-small" onClick={copyLog}>
              Copy
            </button>
          </div>
          <ol>
            {log.map((l, i) => (
              <li key={i} className={`fw-log-${l.level}`}>
                {l.message}
              </li>
            ))}
          </ol>
        </section>
      )}
    </div>
  );
}
