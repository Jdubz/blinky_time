import { useCallback, useRef, useEffect } from 'react';
import { DeviceSetting, SettingsByCategory } from '../types';
import { settingsMetadata } from '../data/settingsMetadata';

interface SettingsPanelProps {
  settingsByCategory: SettingsByCategory;
  onSettingChange: (name: string, value: number | boolean) => void;
  onSave: () => void;
  onLoad: () => void;
  onReset: () => void;
  onRefresh: () => void;
  disabled: boolean;
}

interface SettingControlProps {
  setting: DeviceSetting;
  onChange: (value: number | boolean) => void;
  disabled: boolean;
}

function SettingControl({ setting, onChange, disabled }: SettingControlProps) {
  const debounceRef = useRef<ReturnType<typeof setTimeout>>();

  // Cleanup timeout on unmount to prevent memory leaks
  useEffect(() => {
    return () => {
      if (debounceRef.current) {
        clearTimeout(debounceRef.current);
      }
    };
  }, []);

  const handleChange = useCallback(
    (value: number | boolean) => {
      // Debounce changes to avoid flooding serial
      if (debounceRef.current) {
        clearTimeout(debounceRef.current);
      }
      debounceRef.current = setTimeout(() => {
        onChange(value);
      }, 100);
    },
    [onChange]
  );

  // Boolean toggle
  if (setting.type === 'bool') {
    const metadata = settingsMetadata[setting.name];
    const displayName = metadata?.displayName || setting.desc || setting.name;
    const tooltip = metadata?.tooltip || setting.desc || '';

    return (
      <div className="setting-control">
        <label className="setting-label" title={tooltip}>
          {displayName}
        </label>
        <label className="toggle">
          <input
            type="checkbox"
            checked={setting.value as boolean}
            onChange={e => handleChange(e.target.checked)}
            disabled={disabled}
          />
          <span className="toggle-slider" />
        </label>
      </div>
    );
  }

  // Numeric input (no slider - allows any value within technical limits)
  const numValue = setting.value as number;
  const step = setting.type === 'float' ? 0.01 : 1;

  const metadata = settingsMetadata[setting.name];
  const displayName = metadata?.displayName || setting.desc || setting.name;
  const tooltip = metadata?.tooltip || setting.desc || '';

  return (
    <div className="setting-control">
      <label className="setting-label" title={tooltip}>
        {displayName}
      </label>
      <div className="setting-slider-group">
        <input
          type="number"
          className="setting-input"
          min={setting.min}
          max={setting.max}
          step={step}
          value={numValue}
          onChange={e => handleChange(parseFloat(e.target.value))}
          disabled={disabled}
        />
        {metadata?.unit && <span className="setting-unit">{metadata.unit}</span>}
      </div>
    </div>
  );
}

// Category display names (architecture-based)
const categoryNames: Record<string, string> = {
  audio: 'Audio Processing',
  agc: 'Auto-Gain Control',
  transient: 'Transient Detection',
  detection: 'Detection Algorithms',
  music: 'Music Mode (PLL)',
  rhythm: 'Rhythm Analyzer',
  fire: 'Fire Generator',
  firemusic: 'Fire: Music Mode',
  fireorganic: 'Fire: Organic Mode',
  water: 'Water Generator',
  lightning: 'Lightning Generator',
  audiovis: 'Audio Visualization',
  effect: 'Post-Processing Effects',
};

// Category descriptions
const categoryDescriptions: Record<string, string> = {
  audio: 'AdaptiveMic input processing → Produces: Level, Transient, Envelope, Gain',
  agc: 'Hardware gain control (optimizes ADC signal quality)',
  transient: 'Core transient detection parameters (LOUD + SUDDEN + INFREQUENT)',
  detection: 'Algorithm selection and mode-specific parameters (Drummer, Bass, HFC, Flux, Hybrid)',
  music: 'Phase-locked loop beat tracking → Produces: BPM, Phase, Confidence, Beat Events',
  rhythm: 'Autocorrelation-based tempo detection → Produces: BPM, Periodicity, Beat Likelihood',
  fire: 'Particle-based fire simulation with sparks, heat diffusion, and gravity physics',
  firemusic: 'Fire behavior when rhythm tracking is active (beat-synced bursts, downbeat emphasis)',
  fireorganic: 'Fire behavior when no rhythm detected (transient-reactive, organic spawning)',
  water: 'Particle-based water simulation with drops, splashes, and radial physics',
  lightning: 'Particle-based lightning with fast bolts, branching patterns, and rapid fade',
  audiovis:
    'Diagnostic visualization of transients (green), energy level (yellow), and beat phase (blue)',
  effect: 'Color and post-processing effects applied after generator rendering',
};

// Category order for display (input processing → detection → musical analysis → generators → effects)
const categoryOrder = [
  'audio',
  'agc',
  'transient',
  'detection',
  'music',
  'rhythm',
  'fire',
  'firemusic',
  'fireorganic',
  'water',
  'lightning',
  'audiovis',
  'effect',
];

export function SettingsPanel({
  settingsByCategory,
  onSettingChange,
  onSave,
  onLoad,
  onReset,
  onRefresh,
  disabled,
}: SettingsPanelProps) {
  const categories = Object.keys(settingsByCategory);

  // Sort categories by preferred order
  const sortedCategories = categories.sort((a, b) => {
    const aIndex = categoryOrder.indexOf(a);
    const bIndex = categoryOrder.indexOf(b);
    if (aIndex === -1 && bIndex === -1) return a.localeCompare(b);
    if (aIndex === -1) return 1;
    if (bIndex === -1) return -1;
    return aIndex - bIndex;
  });

  if (categories.length === 0) {
    return (
      <div className="settings-panel">
        <div className="settings-header">
          <h2>Settings</h2>
        </div>
        <div className="settings-empty">
          {disabled ? 'Connect to device to see settings' : 'No settings available'}
        </div>
      </div>
    );
  }

  return (
    <div className="settings-panel">
      <div className="settings-header">
        <h2>Settings</h2>
        <div className="settings-actions">
          <button className="btn btn-small" onClick={onRefresh} disabled={disabled}>
            Refresh
          </button>
          <button className="btn btn-small" onClick={onLoad} disabled={disabled}>
            Load
          </button>
          <button className="btn btn-small btn-primary" onClick={onSave} disabled={disabled}>
            Save
          </button>
          <button className="btn btn-small btn-danger" onClick={onReset} disabled={disabled}>
            Reset
          </button>
        </div>
      </div>

      <div className="settings-categories">
        {sortedCategories.map(category => (
          <div key={category} className="settings-category">
            <h3 className="category-title">{categoryNames[category] || category.toUpperCase()}</h3>
            {categoryDescriptions[category] && (
              <p className="category-description">{categoryDescriptions[category]}</p>
            )}
            <div className="category-settings">
              {settingsByCategory[category].map((setting: DeviceSetting) => (
                <SettingControl
                  key={setting.name}
                  setting={setting}
                  onChange={value => onSettingChange(setting.name, value)}
                  disabled={disabled}
                />
              ))}
            </div>
          </div>
        ))}
      </div>
    </div>
  );
}
