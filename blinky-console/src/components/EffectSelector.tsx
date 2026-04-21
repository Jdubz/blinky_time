/**
 * EffectSelector — three-mode post-processing effect picker.
 *
 * Modes:
 *   off    — `effect none` on firmware. No post-processing.
 *   rotate — `effect hue` + nonzero `huespeed`. Continuous rainbow rotation.
 *   static — `effect hue` + `huespeed=0` + `hueshift=<value>`. Fixed hue offset.
 *
 * Parent owns mode/speed/hue state and handles the firmware command mapping
 * (see MainShell). This component is pure UI.
 */
import { useMemo } from 'react';

export type EffectMode = 'off' | 'rotate' | 'static';

interface EffectSelectorProps {
  mode: EffectMode;
  /** Rotation speed in cycles/sec, 0–2 (firmware `huespeed`). */
  speed: number;
  /** Static hue offset, 0–1 (firmware `hueshift`). Displayed 0–360°. */
  hue: number;
  onModeChange: (mode: EffectMode) => void;
  onSpeedChange: (speed: number) => void;
  onHueChange: (hue: number) => void;
  disabled?: boolean;
}

const MODES: { id: EffectMode; label: string; description: string }[] = [
  { id: 'off', label: 'Off', description: 'Generator colors as-is' },
  { id: 'rotate', label: 'Rotate', description: 'Continuous rainbow cycle' },
  { id: 'static', label: 'Static', description: 'Fixed hue shift (pick a color)' },
];

export function EffectSelector({
  mode,
  speed,
  hue,
  onModeChange,
  onSpeedChange,
  onHueChange,
  disabled = false,
}: EffectSelectorProps) {
  // Gradient background for the hue slider so the user sees the rainbow.
  const hueGradient = useMemo(
    () =>
      'linear-gradient(to right, ' +
      [0, 60, 120, 180, 240, 300, 360]
        .map(h => `hsl(${h}, 100%, 50%) ${((h / 360) * 100).toFixed(0)}%`)
        .join(', ') +
      ')',
    []
  );

  const hueDegrees = Math.round(hue * 360);

  return (
    <div className="effect-selector">
      <div className="selector-header">
        <h3>Effect</h3>
        <span className="selector-hint">Post-processing on generator output</span>
      </div>

      <div className="effect-mode-segmented" role="radiogroup" aria-label="Effect mode">
        {MODES.map(m => (
          <button
            key={m.id}
            type="button"
            role="radio"
            aria-checked={mode === m.id}
            className={`effect-mode-button ${mode === m.id ? 'active' : ''}`}
            onClick={() => onModeChange(m.id)}
            disabled={disabled}
            title={m.description}
          >
            {m.label}
          </button>
        ))}
      </div>

      {mode === 'rotate' && (
        <div className="effect-param-row">
          <label className="effect-param-label" htmlFor="hue-speed">
            Speed
            <span className="effect-param-value">{speed.toFixed(2)} Hz</span>
          </label>
          <input
            id="hue-speed"
            className="effect-param-slider"
            type="range"
            min={0.01}
            max={2}
            step={0.01}
            value={speed}
            onChange={e => onSpeedChange(parseFloat(e.target.value))}
            disabled={disabled}
          />
        </div>
      )}

      {mode === 'static' && (
        <div className="effect-param-row">
          <label className="effect-param-label" htmlFor="hue-shift">
            Hue
            <span
              className="effect-param-value effect-hue-swatch"
              style={{ background: `hsl(${hueDegrees}, 100%, 50%)` }}
              aria-label={`Current hue ${hueDegrees}°`}
            >
              {hueDegrees}°
            </span>
          </label>
          <input
            id="hue-shift"
            className="effect-param-slider effect-hue-slider"
            type="range"
            min={0}
            max={1}
            step={0.002}
            value={hue}
            onChange={e => onHueChange(parseFloat(e.target.value))}
            disabled={disabled}
            style={{ background: hueGradient }}
          />
        </div>
      )}
    </div>
  );
}
