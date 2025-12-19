import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render, screen, fireEvent, waitFor } from '@testing-library/react';
import { SettingsPanel } from './SettingsPanel';
import { DeviceSetting, SettingsByCategory } from '../types';

describe('SettingsPanel', () => {
  const mockSettings: DeviceSetting[] = [
    { name: 'intensity', value: 0.75, type: 'float', cat: 'fire', min: 0, max: 1 },
    { name: 'speed', value: 100, type: 'uint8', cat: 'fire', min: 0, max: 255 },
    { name: 'enabled', value: true, type: 'bool', cat: 'audio', min: 0, max: 1 },
    { name: 'gain', value: 2.5, type: 'float', cat: 'agc', min: 0.1, max: 10 },
    { name: 'debug', value: false, type: 'bool', cat: 'debug', min: 0, max: 1 },
  ];

  const mockSettingsByCategory: SettingsByCategory = {
    fire: mockSettings.filter(s => s.cat === 'fire'),
    audio: mockSettings.filter(s => s.cat === 'audio'),
    agc: mockSettings.filter(s => s.cat === 'agc'),
    debug: mockSettings.filter(s => s.cat === 'debug'),
  };

  const defaultProps = {
    settingsByCategory: mockSettingsByCategory,
    onSettingChange: vi.fn(),
    onSave: vi.fn(),
    onLoad: vi.fn(),
    onReset: vi.fn(),
    onRefresh: vi.fn(),
    disabled: false,
  };

  beforeEach(() => {
    vi.clearAllMocks();
    vi.useFakeTimers();
  });

  it('renders the settings header', () => {
    render(<SettingsPanel {...defaultProps} />);
    expect(screen.getByText('Settings')).toBeInTheDocument();
  });

  describe('empty state', () => {
    it('shows placeholder when disabled and no settings', () => {
      render(<SettingsPanel {...defaultProps} settingsByCategory={{}} disabled={true} />);
      expect(screen.getByText('Connect to device to see settings')).toBeInTheDocument();
    });

    it('shows placeholder when enabled and no settings', () => {
      render(<SettingsPanel {...defaultProps} settingsByCategory={{}} />);
      expect(screen.getByText('No settings available')).toBeInTheDocument();
    });
  });

  describe('action buttons', () => {
    it('renders all action buttons', () => {
      render(<SettingsPanel {...defaultProps} />);
      expect(screen.getByRole('button', { name: 'Refresh' })).toBeInTheDocument();
      expect(screen.getByRole('button', { name: 'Load' })).toBeInTheDocument();
      expect(screen.getByRole('button', { name: 'Save' })).toBeInTheDocument();
      expect(screen.getByRole('button', { name: 'Reset' })).toBeInTheDocument();
    });

    it('calls onRefresh when Refresh is clicked', () => {
      const onRefresh = vi.fn();
      render(<SettingsPanel {...defaultProps} onRefresh={onRefresh} />);
      fireEvent.click(screen.getByRole('button', { name: 'Refresh' }));
      expect(onRefresh).toHaveBeenCalledTimes(1);
    });

    it('calls onLoad when Load is clicked', () => {
      const onLoad = vi.fn();
      render(<SettingsPanel {...defaultProps} onLoad={onLoad} />);
      fireEvent.click(screen.getByRole('button', { name: 'Load' }));
      expect(onLoad).toHaveBeenCalledTimes(1);
    });

    it('calls onSave when Save is clicked', () => {
      const onSave = vi.fn();
      render(<SettingsPanel {...defaultProps} onSave={onSave} />);
      fireEvent.click(screen.getByRole('button', { name: 'Save' }));
      expect(onSave).toHaveBeenCalledTimes(1);
    });

    it('calls onReset when Reset is clicked', () => {
      const onReset = vi.fn();
      render(<SettingsPanel {...defaultProps} onReset={onReset} />);
      fireEvent.click(screen.getByRole('button', { name: 'Reset' }));
      expect(onReset).toHaveBeenCalledTimes(1);
    });

    it('disables all buttons when disabled', () => {
      render(<SettingsPanel {...defaultProps} disabled={true} />);
      expect(screen.getByRole('button', { name: 'Refresh' })).toBeDisabled();
      expect(screen.getByRole('button', { name: 'Load' })).toBeDisabled();
      expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
      expect(screen.getByRole('button', { name: 'Reset' })).toBeDisabled();
    });
  });

  describe('category display', () => {
    it('renders all categories with proper names', () => {
      render(<SettingsPanel {...defaultProps} />);
      expect(screen.getByText('Fire Effect')).toBeInTheDocument();
      expect(screen.getByText('Audio Input')).toBeInTheDocument();
      expect(screen.getByText('Auto Gain Control')).toBeInTheDocument();
      expect(screen.getByText('Debug')).toBeInTheDocument();
    });

    it('renders categories in correct order', () => {
      render(<SettingsPanel {...defaultProps} />);
      const categoryTitles = screen.getAllByRole('heading', { level: 3 });
      const titles = categoryTitles.map(h => h.textContent);

      // fire, audio, agc, debug is the expected order
      expect(titles.indexOf('Fire Effect')).toBeLessThan(titles.indexOf('Audio Input'));
      expect(titles.indexOf('Audio Input')).toBeLessThan(titles.indexOf('Auto Gain Control'));
    });
  });

  describe('setting controls', () => {
    it('renders boolean settings as checkboxes', () => {
      render(<SettingsPanel {...defaultProps} />);
      const checkboxes = screen.getAllByRole('checkbox');
      expect(checkboxes.length).toBe(2); // enabled and debug
    });

    it('renders numeric settings as sliders', () => {
      render(<SettingsPanel {...defaultProps} />);
      const sliders = screen.getAllByRole('slider');
      expect(sliders.length).toBe(3); // intensity, speed, gain
    });

    it('displays setting names', () => {
      render(<SettingsPanel {...defaultProps} />);
      expect(screen.getByText('intensity')).toBeInTheDocument();
      expect(screen.getByText('speed')).toBeInTheDocument();
      expect(screen.getByText('enabled')).toBeInTheDocument();
      expect(screen.getByText('gain')).toBeInTheDocument();
    });

    it('displays current values for float settings', () => {
      render(<SettingsPanel {...defaultProps} />);
      expect(screen.getByText('0.75')).toBeInTheDocument();
      expect(screen.getByText('2.50')).toBeInTheDocument();
    });

    it('displays current values for integer settings', () => {
      render(<SettingsPanel {...defaultProps} />);
      expect(screen.getByText('100')).toBeInTheDocument();
    });
  });

  describe('setting interactions', () => {
    it('calls onSettingChange when checkbox is toggled', async () => {
      const onSettingChange = vi.fn();
      render(<SettingsPanel {...defaultProps} onSettingChange={onSettingChange} />);

      const checkboxes = screen.getAllByRole('checkbox');
      fireEvent.click(checkboxes[0]); // Toggle 'enabled'

      // Wait for debounce
      vi.advanceTimersByTime(150);

      await waitFor(() => {
        expect(onSettingChange).toHaveBeenCalledWith('enabled', false);
      });
    });

    it('calls onSettingChange when slider is changed', async () => {
      const onSettingChange = vi.fn();
      render(<SettingsPanel {...defaultProps} onSettingChange={onSettingChange} />);

      const sliders = screen.getAllByRole('slider');
      fireEvent.change(sliders[0], { target: { value: '0.5' } });

      // Wait for debounce
      vi.advanceTimersByTime(150);

      await waitFor(() => {
        expect(onSettingChange).toHaveBeenCalledWith('intensity', 0.5);
      });
    });

    it('debounces rapid slider changes', async () => {
      const onSettingChange = vi.fn();
      render(<SettingsPanel {...defaultProps} onSettingChange={onSettingChange} />);

      const sliders = screen.getAllByRole('slider');

      // Rapid changes
      fireEvent.change(sliders[0], { target: { value: '0.1' } });
      fireEvent.change(sliders[0], { target: { value: '0.2' } });
      fireEvent.change(sliders[0], { target: { value: '0.3' } });
      fireEvent.change(sliders[0], { target: { value: '0.4' } });

      // Wait for debounce
      vi.advanceTimersByTime(150);

      await waitFor(() => {
        // Should only call once with the final value
        expect(onSettingChange).toHaveBeenCalledTimes(1);
        expect(onSettingChange).toHaveBeenCalledWith('intensity', 0.4);
      });
    });

    it('disables controls when disabled prop is true', () => {
      render(<SettingsPanel {...defaultProps} disabled={true} />);

      const checkboxes = screen.getAllByRole('checkbox');
      const sliders = screen.getAllByRole('slider');

      checkboxes.forEach(checkbox => {
        expect(checkbox).toBeDisabled();
      });

      sliders.forEach(slider => {
        expect(slider).toBeDisabled();
      });
    });
  });

  describe('slider configuration', () => {
    it('sets correct min/max for sliders', () => {
      render(<SettingsPanel {...defaultProps} />);

      const sliders = screen.getAllByRole('slider');
      const intensitySlider = sliders[0];

      expect(intensitySlider).toHaveAttribute('min', '0');
      expect(intensitySlider).toHaveAttribute('max', '1');
    });

    it('uses correct step for float settings', () => {
      render(<SettingsPanel {...defaultProps} />);

      const sliders = screen.getAllByRole('slider');
      const intensitySlider = sliders[0]; // float type

      expect(intensitySlider).toHaveAttribute('step', '0.01');
    });

    it('uses correct step for integer settings', () => {
      render(<SettingsPanel {...defaultProps} />);

      const sliders = screen.getAllByRole('slider');
      const speedSlider = sliders[1]; // uint8 type

      expect(speedSlider).toHaveAttribute('step', '1');
    });
  });
});
