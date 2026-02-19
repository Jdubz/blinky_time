import { describe, it, expect, vi } from 'vitest';
import { render, screen, fireEvent } from '@testing-library/react';
import { GeneratorSelector } from './GeneratorSelector';
import type { GeneratorType } from '../types';

describe('GeneratorSelector', () => {
  const defaultProps = {
    currentGenerator: 'fire' as GeneratorType,
    availableGenerators: ['fire', 'water', 'lightning', 'audio'] as GeneratorType[],
    onGeneratorChange: vi.fn(),
  };

  beforeEach(() => {
    vi.clearAllMocks();
  });

  describe('rendering', () => {
    it('renders all available generator buttons', () => {
      render(<GeneratorSelector {...defaultProps} />);

      expect(screen.getByRole('button', { name: /fire/i })).toBeInTheDocument();
      expect(screen.getByRole('button', { name: /water/i })).toBeInTheDocument();
      expect(screen.getByRole('button', { name: /lightning/i })).toBeInTheDocument();
    });

    it('renders with correct header', () => {
      render(<GeneratorSelector {...defaultProps} />);

      expect(screen.getByText('Active Generator')).toBeInTheDocument();
      expect(screen.getByText('Choose the visual effect pattern')).toBeInTheDocument();
    });

    it('displays generator icons', () => {
      render(<GeneratorSelector {...defaultProps} />);

      expect(screen.getByText('🔥')).toBeInTheDocument();
      expect(screen.getByText('💧')).toBeInTheDocument();
      expect(screen.getByText('⚡')).toBeInTheDocument();
    });

    it('only renders provided generators', () => {
      render(
        <GeneratorSelector
          {...defaultProps}
          availableGenerators={['fire', 'water'] as GeneratorType[]}
        />
      );

      expect(screen.getByRole('button', { name: /fire/i })).toBeInTheDocument();
      expect(screen.getByRole('button', { name: /water/i })).toBeInTheDocument();
      expect(screen.queryByRole('button', { name: /lightning/i })).not.toBeInTheDocument();
    });
  });

  describe('active state', () => {
    it('marks current generator as active', () => {
      render(<GeneratorSelector {...defaultProps} currentGenerator="fire" />);

      const fireButton = screen.getByRole('button', { name: /fire/i });
      const waterButton = screen.getByRole('button', { name: /water/i });

      expect(fireButton).toHaveClass('active');
      expect(waterButton).not.toHaveClass('active');
    });

    it('updates active state when currentGenerator changes', () => {
      const { rerender } = render(<GeneratorSelector {...defaultProps} currentGenerator="fire" />);

      expect(screen.getByRole('button', { name: /fire/i })).toHaveClass('active');

      rerender(<GeneratorSelector {...defaultProps} currentGenerator="water" />);

      expect(screen.getByRole('button', { name: /fire/i })).not.toHaveClass('active');
      expect(screen.getByRole('button', { name: /water/i })).toHaveClass('active');
    });
  });

  describe('interaction', () => {
    it('calls onGeneratorChange when a button is clicked', () => {
      const mockOnChange = vi.fn();
      render(<GeneratorSelector {...defaultProps} onGeneratorChange={mockOnChange} />);

      fireEvent.click(screen.getByRole('button', { name: /water/i }));

      expect(mockOnChange).toHaveBeenCalledTimes(1);
      expect(mockOnChange).toHaveBeenCalledWith('water');
    });

    it('calls onGeneratorChange with correct generator type', () => {
      const mockOnChange = vi.fn();
      render(<GeneratorSelector {...defaultProps} onGeneratorChange={mockOnChange} />);

      fireEvent.click(screen.getByRole('button', { name: /lightning/i }));

      expect(mockOnChange).toHaveBeenCalledWith('lightning');
    });

    it('allows clicking on already active generator', () => {
      const mockOnChange = vi.fn();
      render(
        <GeneratorSelector
          {...defaultProps}
          currentGenerator="fire"
          onGeneratorChange={mockOnChange}
        />
      );

      fireEvent.click(screen.getByRole('button', { name: /fire/i }));

      expect(mockOnChange).toHaveBeenCalledWith('fire');
    });
  });

  describe('disabled state', () => {
    it('disables all buttons when disabled prop is true', () => {
      render(<GeneratorSelector {...defaultProps} disabled={true} />);

      expect(screen.getByRole('button', { name: /fire/i })).toBeDisabled();
      expect(screen.getByRole('button', { name: /water/i })).toBeDisabled();
      expect(screen.getByRole('button', { name: /lightning/i })).toBeDisabled();
    });

    it('enables all buttons when disabled prop is false', () => {
      render(<GeneratorSelector {...defaultProps} disabled={false} />);

      expect(screen.getByRole('button', { name: /fire/i })).not.toBeDisabled();
      expect(screen.getByRole('button', { name: /water/i })).not.toBeDisabled();
      expect(screen.getByRole('button', { name: /lightning/i })).not.toBeDisabled();
    });

    it('does not call onGeneratorChange when disabled', () => {
      const mockOnChange = vi.fn();
      render(
        <GeneratorSelector {...defaultProps} onGeneratorChange={mockOnChange} disabled={true} />
      );

      fireEvent.click(screen.getByRole('button', { name: /water/i }));

      expect(mockOnChange).not.toHaveBeenCalled();
    });
  });

  describe('accessibility', () => {
    it('has tooltips with descriptions', () => {
      render(<GeneratorSelector {...defaultProps} />);

      const fireButton = screen.getByRole('button', { name: /fire/i });
      expect(fireButton).toHaveAttribute(
        'title',
        'Realistic fire simulation with sparks and embers'
      );

      const waterButton = screen.getByRole('button', { name: /water/i });
      expect(waterButton).toHaveAttribute('title', 'Flowing water with waves and ripples');

      const lightningButton = screen.getByRole('button', { name: /lightning/i });
      expect(lightningButton).toHaveAttribute('title', 'Electric bolts with branching patterns');
    });
  });

  describe('audio generator', () => {
    it('renders audio generator when included in available generators', () => {
      render(
        <GeneratorSelector
          {...defaultProps}
          availableGenerators={['fire', 'water', 'lightning', 'audio'] as GeneratorType[]}
        />
      );

      expect(screen.getByRole('button', { name: /audio/i })).toBeInTheDocument();
      expect(screen.getByText('📊')).toBeInTheDocument();
    });

    it('has correct tooltip for audio generator', () => {
      render(
        <GeneratorSelector
          {...defaultProps}
          availableGenerators={['fire', 'water', 'lightning', 'audio'] as GeneratorType[]}
        />
      );

      const audioButton = screen.getByRole('button', { name: /audio/i });
      expect(audioButton).toHaveAttribute(
        'title',
        'Diagnostic audio visualization: transients, level, and beat phase'
      );
    });

    it('calls onGeneratorChange with audio when clicked', () => {
      const mockOnChange = vi.fn();
      render(
        <GeneratorSelector
          {...defaultProps}
          availableGenerators={['fire', 'water', 'lightning', 'audio'] as GeneratorType[]}
          onGeneratorChange={mockOnChange}
        />
      );

      fireEvent.click(screen.getByRole('button', { name: /audio/i }));

      expect(mockOnChange).toHaveBeenCalledWith('audio');
    });
  });
});
