/**
 * EffectSelector tests — replaces the pre-PR-131 tests that were deleted
 * when the component was rewritten around segmented modes. Added
 * 2026-04-24 per review feedback.
 */
import { describe, it, expect, vi } from 'vitest';
import { render, screen, fireEvent } from '@testing-library/react';
import { EffectSelector } from './EffectSelector';

function setup(overrides: Partial<Parameters<typeof EffectSelector>[0]> = {}) {
  const onModeChange = vi.fn();
  const onSpeedChange = vi.fn();
  const onHueChange = vi.fn();
  const utils = render(
    <EffectSelector
      mode="off"
      speed={0.5}
      hue={0}
      onModeChange={onModeChange}
      onSpeedChange={onSpeedChange}
      onHueChange={onHueChange}
      {...overrides}
    />
  );
  return { ...utils, onModeChange, onSpeedChange, onHueChange };
}

describe('EffectSelector mode buttons', () => {
  it('renders three segmented radio buttons', () => {
    setup();
    const radios = screen.getAllByRole('radio');
    expect(radios).toHaveLength(3);
    expect(radios.map(r => r.textContent)).toEqual(['Off', 'Rotate', 'Static']);
  });

  it('marks the active mode with aria-checked=true', () => {
    setup({ mode: 'rotate' });
    const rotate = screen.getByRole('radio', { name: /rotate/i });
    expect(rotate).toHaveAttribute('aria-checked', 'true');
    expect(screen.getByRole('radio', { name: /off/i })).toHaveAttribute('aria-checked', 'false');
  });

  it('calls onModeChange when a button is clicked', () => {
    const { onModeChange } = setup({ mode: 'off' });
    fireEvent.click(screen.getByRole('radio', { name: /rotate/i }));
    expect(onModeChange).toHaveBeenCalledWith('rotate');
  });

  it('roving tabIndex: selected is 0, others are -1', () => {
    setup({ mode: 'rotate' });
    expect(screen.getByRole('radio', { name: /off/i })).toHaveAttribute('tabindex', '-1');
    expect(screen.getByRole('radio', { name: /rotate/i })).toHaveAttribute('tabindex', '0');
    expect(screen.getByRole('radio', { name: /static/i })).toHaveAttribute('tabindex', '-1');
  });
});

describe('EffectSelector keyboard navigation', () => {
  it('ArrowRight moves selection cyclically', () => {
    const { onModeChange } = setup({ mode: 'off' });
    fireEvent.keyDown(screen.getByRole('radio', { name: /off/i }), { key: 'ArrowRight' });
    expect(onModeChange).toHaveBeenCalledWith('rotate');
  });

  it('ArrowLeft wraps around to the last mode', () => {
    const { onModeChange } = setup({ mode: 'off' });
    fireEvent.keyDown(screen.getByRole('radio', { name: /off/i }), { key: 'ArrowLeft' });
    expect(onModeChange).toHaveBeenCalledWith('static');
  });

  it('Home jumps to the first mode', () => {
    const { onModeChange } = setup({ mode: 'static' });
    fireEvent.keyDown(screen.getByRole('radio', { name: /static/i }), { key: 'Home' });
    expect(onModeChange).toHaveBeenCalledWith('off');
  });

  it('End jumps to the last mode', () => {
    const { onModeChange } = setup({ mode: 'off' });
    fireEvent.keyDown(screen.getByRole('radio', { name: /off/i }), { key: 'End' });
    expect(onModeChange).toHaveBeenCalledWith('static');
  });

  it('ignores keys when disabled', () => {
    const { onModeChange } = setup({ mode: 'off', disabled: true });
    fireEvent.keyDown(screen.getByRole('radio', { name: /off/i }), { key: 'ArrowRight' });
    expect(onModeChange).not.toHaveBeenCalled();
  });
});

describe('EffectSelector speed slider (rotate mode only)', () => {
  it('hidden in off mode', () => {
    setup({ mode: 'off' });
    expect(screen.queryByLabelText(/speed/i)).toBeNull();
  });

  it('hidden in static mode', () => {
    setup({ mode: 'static' });
    expect(screen.queryByLabelText(/speed/i)).toBeNull();
  });

  it('visible in rotate mode', () => {
    setup({ mode: 'rotate' });
    expect(screen.getByLabelText(/speed/i)).toBeInTheDocument();
  });

  it('emits parsed float via onSpeedChange', () => {
    const { onSpeedChange } = setup({ mode: 'rotate' });
    const slider = screen.getByLabelText(/speed/i);
    fireEvent.change(slider, { target: { value: '1.25' } });
    expect(onSpeedChange).toHaveBeenCalledWith(1.25);
  });
});

describe('EffectSelector hue slider (static mode only)', () => {
  it('hidden in off and rotate modes', () => {
    const { container, rerender } = setup({ mode: 'off' });
    expect(container.querySelector('#hue-shift')).toBeNull();
    rerender(
      <EffectSelector
        mode="rotate"
        speed={0.5}
        hue={0}
        onModeChange={vi.fn()}
        onSpeedChange={vi.fn()}
        onHueChange={vi.fn()}
      />
    );
    expect(container.querySelector('#hue-shift')).toBeNull();
  });

  it('visible in static mode', () => {
    const { container } = setup({ mode: 'static' });
    expect(container.querySelector('#hue-shift')).toBeInTheDocument();
  });

  it('emits parsed float via onHueChange', () => {
    const { container, onHueChange } = setup({ mode: 'static' });
    const slider = container.querySelector('#hue-shift') as HTMLInputElement;
    expect(slider).not.toBeNull();
    fireEvent.change(slider, { target: { value: '0.75' } });
    expect(onHueChange).toHaveBeenCalledWith(0.75);
  });
});

describe('EffectSelector disabled state', () => {
  it('all controls disabled when disabled prop is true', () => {
    setup({ mode: 'rotate', disabled: true });
    for (const radio of screen.getAllByRole('radio')) {
      expect(radio).toBeDisabled();
    }
    expect(screen.getByLabelText(/speed/i)).toBeDisabled();
  });
});
