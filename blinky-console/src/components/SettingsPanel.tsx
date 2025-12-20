import { useCallback, useRef, useEffect } from 'react';
import { DeviceSetting, SettingsByCategory } from '../types';

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
    return (
      <div className="setting-control">
        <label className="setting-label">{setting.name}</label>
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

  // Numeric slider
  const numValue = setting.value as number;
  const step = setting.type === 'float' ? 0.01 : 1;
  const displayValue = setting.type === 'float' ? numValue.toFixed(2) : numValue;

  return (
    <div className="setting-control">
      <label className="setting-label">{setting.name}</label>
      <div className="setting-slider-group">
        <input
          type="range"
          className="setting-slider"
          min={setting.min}
          max={setting.max}
          step={step}
          value={numValue}
          onChange={e => handleChange(parseFloat(e.target.value))}
          disabled={disabled}
        />
        <span className="setting-value">{displayValue}</span>
      </div>
    </div>
  );
}

// Category display names
const categoryNames: Record<string, string> = {
  fire: 'Fire Effect',
  audio: 'Audio Input',
  agc: 'Auto Gain Control',
};

// Category order for display
const categoryOrder = ['fire', 'audio', 'agc'];

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
            <div className="category-settings">
              {settingsByCategory[category].map(setting => (
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
