import { GeneratorType } from '../types';

interface GeneratorSelectorProps {
  currentGenerator: GeneratorType;
  availableGenerators: GeneratorType[];
  onGeneratorChange: (generator: GeneratorType) => void;
  disabled?: boolean;
}

const GENERATOR_INFO: Record<GeneratorType, { label: string; icon: string; description: string }> =
  {
    fire: {
      label: 'Fire',
      icon: '🔥',
      description: 'Realistic fire simulation with sparks and embers',
    },
    water: {
      label: 'Water',
      icon: '💧',
      description: 'Flowing water with waves and ripples',
    },
    lightning: {
      label: 'Lightning',
      icon: '⚡',
      description: 'Electric bolts with branching patterns',
    },
  };

export function GeneratorSelector({
  currentGenerator,
  availableGenerators,
  onGeneratorChange,
  disabled = false,
}: GeneratorSelectorProps) {
  return (
    <div className="generator-selector">
      <div className="selector-header">
        <h3>Active Generator</h3>
        <span className="selector-hint">Choose the visual effect pattern</span>
      </div>
      <div className="selector-buttons">
        {availableGenerators.map(gen => {
          const info = GENERATOR_INFO[gen];
          const isActive = gen === currentGenerator;
          return (
            <button
              key={gen}
              className={`selector-button ${isActive ? 'active' : ''}`}
              onClick={() => onGeneratorChange(gen)}
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
