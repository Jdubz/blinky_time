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
          datasets: [{ data: [] }, { data: [] }, { data: [] }, { data: [] }],
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
    pk: 0.6,
    vl: 0.05,
    raw: 0.25,
    h: 32,
    alive: 1,
    z: 0.15,
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
      expect(screen.getByText('Peak: 0.60')).toBeInTheDocument();
      expect(screen.getByText('Valley: 0.05')).toBeInTheDocument();
      expect(screen.getByText('HW Gain: 32')).toBeInTheDocument();
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

  describe('music mode telemetry', () => {
    // PLP-era firmware (b79+) — no conf, no bc.
    // Each numeric value is distinct so getByText can match unambiguously.
    const plpMusic = {
      a: 1 as const,
      bpm: 124.5,
      ph: 0.42,
      str: 0.78,
      q: 0 as const,
      e: 0.55,
      p: 0.81,
      pp: 0.7,
      od: 3.2,
      nn: 0.36,
      per: 33,
    };

    it('renders BPM, Phase, Strength when music mode is active', () => {
      render(
        <AudioVisualizer
          {...defaultProps}
          isStreaming={true}
          audioData={mockAudioData}
          musicModeData={plpMusic}
        />
      );

      expect(screen.getByText('BPM')).toBeInTheDocument();
      expect(screen.getByText('124.5')).toBeInTheDocument();
      expect(screen.getByText('Phase')).toBeInTheDocument();
      expect(screen.getByText('0.42')).toBeInTheDocument();
      expect(screen.getByText('Strength')).toBeInTheDocument();
      expect(screen.getByText('78%')).toBeInTheDocument();
    });

    it('omits Confidence and Beats rows when firmware does not emit them', () => {
      render(
        <AudioVisualizer
          {...defaultProps}
          isStreaming={true}
          audioData={mockAudioData}
          musicModeData={plpMusic}
        />
      );

      // Regression guard: previously rendered "NaN%" / "undefined" against
      // PLP-era firmware that doesn't emit conf/bc.
      expect(screen.queryByText('Confidence')).not.toBeInTheDocument();
      expect(screen.queryByText('Beats')).not.toBeInTheDocument();
      expect(screen.queryByText(/NaN/)).not.toBeInTheDocument();
    });

    it('renders NN row when nn field is present', () => {
      render(
        <AudioVisualizer
          {...defaultProps}
          isStreaming={true}
          audioData={mockAudioData}
          musicModeData={plpMusic}
        />
      );

      expect(screen.getByText('NN')).toBeInTheDocument();
      expect(screen.getByText('0.36')).toBeInTheDocument();
    });

    it('still renders legacy Confidence and Beats when firmware emits them', () => {
      render(
        <AudioVisualizer
          {...defaultProps}
          isStreaming={true}
          audioData={mockAudioData}
          musicModeData={{ ...plpMusic, conf: 0.85, bc: 12 }}
        />
      );

      expect(screen.getByText('Confidence')).toBeInTheDocument();
      expect(screen.getByText('85%')).toBeInTheDocument();
      expect(screen.getByText('Beats')).toBeInTheDocument();
      expect(screen.getByText('12')).toBeInTheDocument();
    });

    it('hides telemetry when music mode is inactive', () => {
      render(
        <AudioVisualizer
          {...defaultProps}
          isStreaming={true}
          audioData={mockAudioData}
          musicModeData={{ ...plpMusic, a: 0 }}
        />
      );

      expect(screen.getByText('Music')).toBeInTheDocument();
      expect(screen.getByText('Inactive')).toBeInTheDocument();
      expect(screen.queryByText('BPM')).not.toBeInTheDocument();
    });
  });
});
