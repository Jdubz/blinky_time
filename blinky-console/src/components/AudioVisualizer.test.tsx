import { describe, it, expect, vi } from 'vitest';
import { render, screen, fireEvent } from '@testing-library/react';
import { AudioVisualizer } from './AudioVisualizer';
import { AudioSample } from '../types';

// Mock react-chartjs-2
vi.mock('react-chartjs-2', () => ({
  Line: vi.fn(({ ref }) => {
    // Store ref for testing
    if (ref) {
      ref.current = {
        data: {
          labels: [],
          datasets: [{ data: [] }, { data: [] }, { data: [] }],
        },
        update: vi.fn(),
      };
    }
    return <div data-testid="mock-chart">Chart</div>;
  }),
}));

describe('AudioVisualizer', () => {
  const mockAudioData: AudioSample = {
    l: 0.75,
    t: 0.5,
    e: 0.6,
    g: 2.0,
  };

  const defaultProps = {
    audioData: null,
    batteryData: null,
    batteryStatusData: null,
    isStreaming: false,
    onToggleStreaming: vi.fn(),
    onRequestBatteryStatus: vi.fn(),
    disabled: false,
  };

  it('renders the audio monitor header', () => {
    render(<AudioVisualizer {...defaultProps} />);
    expect(screen.getByText('AdaptiveMic Output')).toBeInTheDocument();
    expect(screen.getByText('Inputs to Fire Generator')).toBeInTheDocument();
  });

  describe('streaming controls', () => {
    it('shows "Start Stream" button when not streaming', () => {
      render(<AudioVisualizer {...defaultProps} />);
      expect(screen.getByRole('button', { name: 'Start Stream' })).toBeInTheDocument();
    });

    it('shows "Stop Stream" button when streaming', () => {
      render(<AudioVisualizer {...defaultProps} isStreaming={true} />);
      expect(screen.getByRole('button', { name: 'Stop Stream' })).toBeInTheDocument();
    });

    it('calls onToggleStreaming when stream button is clicked', () => {
      const onToggleStreaming = vi.fn();
      render(<AudioVisualizer {...defaultProps} onToggleStreaming={onToggleStreaming} />);

      fireEvent.click(screen.getByRole('button', { name: 'Start Stream' }));
      expect(onToggleStreaming).toHaveBeenCalledTimes(1);
    });

    it('disables stream button when disabled', () => {
      render(<AudioVisualizer {...defaultProps} disabled={true} />);
      expect(screen.getByRole('button', { name: 'Start Stream' })).toBeDisabled();
    });
  });

  describe('clear button', () => {
    it('shows Clear button', () => {
      render(<AudioVisualizer {...defaultProps} />);
      expect(screen.getByRole('button', { name: 'Clear' })).toBeInTheDocument();
    });

    it('disables Clear button when not streaming', () => {
      render(<AudioVisualizer {...defaultProps} />);
      expect(screen.getByRole('button', { name: 'Clear' })).toBeDisabled();
    });

    it('enables Clear button when streaming', () => {
      render(<AudioVisualizer {...defaultProps} isStreaming={true} />);
      expect(screen.getByRole('button', { name: 'Clear' })).not.toBeDisabled();
    });

    it('disables Clear button when disabled even if streaming', () => {
      render(<AudioVisualizer {...defaultProps} isStreaming={true} disabled={true} />);
      expect(screen.getByRole('button', { name: 'Clear' })).toBeDisabled();
    });
  });

  describe('audio values display', () => {
    it('shows audio values when streaming with data', () => {
      render(<AudioVisualizer {...defaultProps} isStreaming={true} audioData={mockAudioData} />);

      expect(screen.getByText('Level: 0.75')).toBeInTheDocument();
      expect(screen.getByText('Transient: 0.50')).toBeInTheDocument();
      expect(screen.getByText('AGC Gain: 2.0x')).toBeInTheDocument();
    });

    it('does not show audio values when not streaming', () => {
      render(<AudioVisualizer {...defaultProps} isStreaming={false} audioData={mockAudioData} />);

      expect(screen.queryByText('Level:')).not.toBeInTheDocument();
    });

    it('does not show audio values when no audio data', () => {
      render(<AudioVisualizer {...defaultProps} isStreaming={true} audioData={null} />);

      expect(screen.queryByText('Level:')).not.toBeInTheDocument();
    });
  });

  describe('placeholder states', () => {
    it('shows placeholder when not streaming and not disabled', () => {
      render(<AudioVisualizer {...defaultProps} />);
      expect(screen.getByText('Click "Start Stream" to visualize audio input')).toBeInTheDocument();
    });

    it('shows disabled placeholder when disabled', () => {
      render(<AudioVisualizer {...defaultProps} disabled={true} />);
      expect(screen.getByText('Connect to device to monitor audio')).toBeInTheDocument();
    });

    it('does not show placeholder when streaming', () => {
      render(<AudioVisualizer {...defaultProps} isStreaming={true} />);
      expect(screen.queryByText('Click "Start Stream"')).not.toBeInTheDocument();
    });
  });

  describe('chart rendering', () => {
    it('renders the chart component', () => {
      render(<AudioVisualizer {...defaultProps} />);
      expect(screen.getByTestId('mock-chart')).toBeInTheDocument();
    });
  });

  describe('button styling', () => {
    it('uses danger style for Stop button', () => {
      render(<AudioVisualizer {...defaultProps} isStreaming={true} />);
      const stopButton = screen.getByRole('button', { name: 'Stop Stream' });
      expect(stopButton).toHaveClass('btn-danger');
    });

    it('uses primary style for Start button', () => {
      render(<AudioVisualizer {...defaultProps} />);
      const startButton = screen.getByRole('button', { name: 'Start Stream' });
      expect(startButton).toHaveClass('btn-primary');
    });
  });
});
