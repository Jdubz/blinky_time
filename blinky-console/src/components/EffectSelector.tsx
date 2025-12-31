import { EffectType } from '../types';

interface EffectSelectorProps {
  currentEffect: EffectType;
  availableEffects: EffectType[];
  onEffectChange: (effect: EffectType) => void;
  disabled?: boolean;
}

const EFFECT_INFO: Record<EffectType, { label: string; icon: string; description: string }> = {
  none: {
    label: 'None',
    icon: '○',
    description: 'No post-processing effect (original colors)',
  },
  hue: {
    label: 'Hue Rotation',
    icon: '🌈',
    description: 'Cycles colors through the rainbow over time',
  },
};

export function EffectSelector({
  currentEffect,
  availableEffects,
  onEffectChange,
  disabled = false,
}: EffectSelectorProps) {
  return (
    <div className="effect-selector">
      <div className="selector-header">
        <h3>Active Effect</h3>
        <span className="selector-hint">Post-processing applied to generator output</span>
      </div>
      <div className="selector-buttons">
        {availableEffects.map(effect => {
          const info = EFFECT_INFO[effect];
          const isActive = effect === currentEffect;
          return (
            <button
              key={effect}
              className={`selector-button ${isActive ? 'active' : ''}`}
              onClick={() => onEffectChange(effect)}
              disabled={disabled}
              title={info.description}
            >
              <span className="selector-icon">{info.icon}</span>
              <span className="selector-label">{info.label}</span>
            </button>
          );
        })}
      </div>
    </div>
  );
}
