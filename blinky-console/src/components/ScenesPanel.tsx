/**
 * ScenesPanel — save, list, load, delete named fleet configurations.
 *
 * Scenes are stored on blinky-server under ~/.local/share/blinky-server/scenes/.
 * The MVP scene captures {generator, effect mode, hue speed, hue shift} — enough
 * to reproduce a look. Applied via POST /api/scenes/{name}/apply which fans out
 * the firmware command sequence to every connected device.
 */
import { useCallback, useEffect, useState } from 'react';
import type { EffectMode } from './EffectSelector';
import type { GeneratorType } from '../types';

export interface Scene {
  name: string;
  generator: GeneratorType;
  effect_mode: EffectMode;
  effect_speed: number;
  effect_hue: number;
}

interface ScenesPanelProps {
  /** Current UI state — used as the starting point for "Save current". */
  currentGenerator: GeneratorType;
  currentEffectMode: EffectMode;
  currentHueSpeed: number;
  currentHueShift: number;
  /** Called after applying a scene so MainShell can resync its own state. */
  onApplied: (scene: Scene) => void;
  disabled?: boolean;
}

async function api<T>(url: string, init?: RequestInit): Promise<T> {
  const resp = await fetch(url, {
    ...init,
    headers: init?.body
      ? { 'Content-Type': 'application/json', ...(init?.headers || {}) }
      : init?.headers,
    signal: AbortSignal.timeout(10_000),
  });
  if (!resp.ok) throw new Error(`${init?.method ?? 'GET'} ${url} → ${resp.status}`);
  return resp.json() as Promise<T>;
}

export function ScenesPanel({
  currentGenerator,
  currentEffectMode,
  currentHueSpeed,
  currentHueShift,
  onApplied,
  disabled = false,
}: ScenesPanelProps) {
  const [scenes, setScenes] = useState<Scene[]>([]);
  const [name, setName] = useState('');
  const [busy, setBusy] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  const refresh = useCallback(async () => {
    try {
      setScenes(await api<Scene[]>('/api/scenes'));
      setError(null);
    } catch (e) {
      setError((e as Error).message);
    }
  }, []);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  const handleSave = async () => {
    const trimmed = name.trim();
    if (!trimmed) return;
    setBusy('save');
    try {
      const scene: Scene = {
        name: trimmed,
        generator: currentGenerator,
        effect_mode: currentEffectMode,
        effect_speed: currentHueSpeed,
        effect_hue: currentHueShift,
      };
      await api<Scene>(`/api/scenes/${encodeURIComponent(trimmed)}`, {
        method: 'PUT',
        body: JSON.stringify(scene),
      });
      setName('');
      await refresh();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(null);
    }
  };

  const handleApply = async (scene: Scene) => {
    setBusy(`apply:${scene.name}`);
    try {
      await api<unknown>(`/api/scenes/${encodeURIComponent(scene.name)}/apply`, {
        method: 'POST',
      });
      onApplied(scene);
      setError(null);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(null);
    }
  };

  const handleDelete = async (scene: Scene) => {
    if (!confirm(`Delete scene "${scene.name}"?`)) return;
    setBusy(`delete:${scene.name}`);
    try {
      await api<unknown>(`/api/scenes/${encodeURIComponent(scene.name)}`, {
        method: 'DELETE',
      });
      await refresh();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(null);
    }
  };

  const modeLabel = (m: EffectMode) =>
    m === 'off' ? 'no effect' : m === 'rotate' ? 'rotate' : 'static hue';

  return (
    <section className="scenes-panel">
      <div className="scenes-header">
        <h3>Scenes</h3>
        <span className="scenes-hint">Save and recall fleet looks</span>
      </div>

      {error && <div className="scenes-error">{error}</div>}

      <div className="scenes-save-row">
        <input
          className="scenes-save-input"
          type="text"
          placeholder="Name the current look…"
          value={name}
          onChange={e => setName(e.target.value)}
          disabled={disabled || busy !== null}
          onKeyDown={e => {
            if (e.key === 'Enter') void handleSave();
          }}
        />
        <button
          type="button"
          className="btn btn-primary"
          onClick={handleSave}
          disabled={disabled || !name.trim() || busy !== null}
        >
          {busy === 'save' ? 'Saving…' : 'Save'}
        </button>
      </div>

      <ul className="scenes-list">
        {scenes.length === 0 && <li className="scenes-empty">No scenes yet.</li>}
        {scenes.map(scene => (
          <li key={scene.name} className="scenes-item">
            <button
              type="button"
              className="scenes-item__apply"
              onClick={() => handleApply(scene)}
              disabled={disabled || busy !== null}
              title={`Apply to all connected devices`}
            >
              <span className="scenes-item__name">{scene.name}</span>
              <span className="scenes-item__meta">
                {scene.generator} · {modeLabel(scene.effect_mode)}
              </span>
            </button>
            <button
              type="button"
              className="scenes-item__delete"
              onClick={() => handleDelete(scene)}
              disabled={disabled || busy !== null}
              aria-label={`Delete scene ${scene.name}`}
            >
              ×
            </button>
          </li>
        ))}
      </ul>
    </section>
  );
}
