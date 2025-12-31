import { describe, it, expect, vi } from 'vitest';
import { render, screen, fireEvent } from '@testing-library/react';
import { EffectSelector } from './EffectSelector';
import type { EffectType } from '../types';

describe('EffectSelector', () => {
  const defaultProps = {
    currentEffect: 'none' as EffectType,
    availableEffects: ['none', 'hue'] as EffectType[],
    onEffectChange: vi.fn(),
  };

  beforeEach(() => {
    vi.clearAllMocks();
  });

  describe('rendering', () => {
    it('renders all available effect buttons', () => {
      render(<EffectSelector {...defaultProps} />);

      expect(screen.getByRole('button', { name: /none/i })).toBeInTheDocument();
      expect(screen.getByRole('button', { name: /hue rotation/i })).toBeInTheDocument();
    });

    it('renders with correct header', () => {
      render(<EffectSelector {...defaultProps} />);

      expect(screen.getByText('Active Effect')).toBeInTheDocument();
      expect(screen.getByText('Post-processing applied to generator output')).toBeInTheDocument();
    });

    it('displays effect icons', () => {
      render(<EffectSelector {...defaultProps} />);

      expect(screen.getByText('○')).toBeInTheDocument();
      expect(screen.getByText('🌈')).toBeInTheDocument();
    });

    it('only renders provided effects', () => {
      render(<EffectSelector {...defaultProps} availableEffects={['none'] as EffectType[]} />);

      expect(screen.getByRole('button', { name: /none/i })).toBeInTheDocument();
      expect(screen.queryByRole('button', { name: /hue rotation/i })).not.toBeInTheDocument();
    });
  });

  describe('active state', () => {
    it('marks current effect as active', () => {
      render(<EffectSelector {...defaultProps} currentEffect="none" />);

      const noneButton = screen.getByRole('button', { name: /none/i });
      const hueButton = screen.getByRole('button', { name: /hue rotation/i });

      expect(noneButton).toHaveClass('active');
      expect(hueButton).not.toHaveClass('active');
    });

    it('updates active state when currentEffect changes', () => {
      const { rerender } = render(<EffectSelector {...defaultProps} currentEffect="none" />);

      expect(screen.getByRole('button', { name: /none/i })).toHaveClass('active');

      rerender(<EffectSelector {...defaultProps} currentEffect="hue" />);

      expect(screen.getByRole('button', { name: /none/i })).not.toHaveClass('active');
      expect(screen.getByRole('button', { name: /hue rotation/i })).toHaveClass('active');
    });
  });

  describe('interaction', () => {
    it('calls onEffectChange when a button is clicked', () => {
      const mockOnChange = vi.fn();
      render(<EffectSelector {...defaultProps} onEffectChange={mockOnChange} />);

      fireEvent.click(screen.getByRole('button', { name: /hue rotation/i }));

      expect(mockOnChange).toHaveBeenCalledTimes(1);
      expect(mockOnChange).toHaveBeenCalledWith('hue');
    });

    it('calls onEffectChange with correct effect type', () => {
      const mockOnChange = vi.fn();
      render(
        <EffectSelector {...defaultProps} currentEffect="hue" onEffectChange={mockOnChange} />
      );

      fireEvent.click(screen.getByRole('button', { name: /none/i }));

      expect(mockOnChange).toHaveBeenCalledWith('none');
    });

    it('allows clicking on already active effect', () => {
      const mockOnChange = vi.fn();
      render(
        <EffectSelector {...defaultProps} currentEffect="none" onEffectChange={mockOnChange} />
      );

      fireEvent.click(screen.getByRole('button', { name: /none/i }));

      expect(mockOnChange).toHaveBeenCalledWith('none');
    });
  });

  describe('disabled state', () => {
    it('disables all buttons when disabled prop is true', () => {
      render(<EffectSelector {...defaultProps} disabled={true} />);

      expect(screen.getByRole('button', { name: /none/i })).toBeDisabled();
      expect(screen.getByRole('button', { name: /hue rotation/i })).toBeDisabled();
    });

    it('enables all buttons when disabled prop is false', () => {
      render(<EffectSelector {...defaultProps} disabled={false} />);

      expect(screen.getByRole('button', { name: /none/i })).not.toBeDisabled();
      expect(screen.getByRole('button', { name: /hue rotation/i })).not.toBeDisabled();
    });

    it('does not call onEffectChange when disabled', () => {
      const mockOnChange = vi.fn();
      render(<EffectSelector {...defaultProps} onEffectChange={mockOnChange} disabled={true} />);

      fireEvent.click(screen.getByRole('button', { name: /hue rotation/i }));

      expect(mockOnChange).not.toHaveBeenCalled();
    });
  });

  describe('accessibility', () => {
    it('has tooltips with descriptions', () => {
      render(<EffectSelector {...defaultProps} />);

      const noneButton = screen.getByRole('button', { name: /none/i });
      expect(noneButton).toHaveAttribute('title', 'No post-processing effect (original colors)');

      const hueButton = screen.getByRole('button', { name: /hue rotation/i });
      expect(hueButton).toHaveAttribute('title', 'Cycles colors through the rainbow over time');
    });
  });
});
