import { useEffect, useRef, useCallback, useState } from 'react';
import {
  Chart as ChartJS,
  CategoryScale,
  LinearScale,
  PointElement,
  LineElement,
  Title,
  Tooltip,
  Legend,
  Filler,
} from 'chart.js';
import { Line } from 'react-chartjs-2';
import { AudioSample } from '../types';
import { audioMetricsMetadata } from '../data/settingsMetadata';

// Register Chart.js components
ChartJS.register(
  CategoryScale,
  LinearScale,
  PointElement,
  LineElement,
  Title,
  Tooltip,
  Legend,
  Filler
);

interface AudioVisualizerProps {
  audioData: AudioSample | null;
  isStreaming: boolean;
  onToggleStreaming: () => void;
  disabled: boolean;
}

interface PercussionEvent {
  index: number;
  type: 'kick' | 'snare' | 'hihat';
  strength: number;
  icon: string;
  color: string;
}

const MAX_DATA_POINTS = 150; // ~7.5 seconds at 20Hz

export function AudioVisualizer({
  audioData,
  isStreaming,
  onToggleStreaming,
  disabled,
}: AudioVisualizerProps) {
  const levelDataRef = useRef<number[]>([]);
  const peakDataRef = useRef<number[]>([]);
  const valleyDataRef = useRef<number[]>([]);
  const labelsRef = useRef<string[]>([]);
  const chartRef = useRef<ChartJS<'line'>>(null);
  const percussionEventsRef = useRef<PercussionEvent[]>([]);
  const chartContainerRef = useRef<HTMLDivElement>(null);
  const [, setRenderTrigger] = useState(0);

  // Update data when new audio sample arrives
  useEffect(() => {
    if (!audioData || !isStreaming) return;

    const currentIndex = levelDataRef.current.length;
    let hasNewPercussion = false;

    // Track percussion events
    if (audioData.k === 1) {
      percussionEventsRef.current.push({
        index: currentIndex,
        type: 'kick',
        strength: audioData.ks,
        icon: '🥁',
        color: '#ef4444',
      });
      hasNewPercussion = true;
    }
    if (audioData.sn === 1) {
      percussionEventsRef.current.push({
        index: currentIndex,
        type: 'snare',
        strength: audioData.ss,
        icon: '🥁',
        color: '#3b82f6',
      });
      hasNewPercussion = true;
    }
    if (audioData.hh === 1) {
      percussionEventsRef.current.push({
        index: currentIndex,
        type: 'hihat',
        strength: audioData.hs,
        icon: '🎵',
        color: '#eab308',
      });
      hasNewPercussion = true;
    }

    if (hasNewPercussion) {
      setRenderTrigger(n => n + 1);
    }

    // Add new data point
    levelDataRef.current.push(audioData.l);
    peakDataRef.current.push(audioData.pk);
    valleyDataRef.current.push(audioData.vl);
    labelsRef.current.push('');

    // Trim to max length
    if (levelDataRef.current.length > MAX_DATA_POINTS) {
      levelDataRef.current.shift();
      peakDataRef.current.shift();
      valleyDataRef.current.shift();
      labelsRef.current.shift();

      // Shift percussion events and remove those that are off the chart
      percussionEventsRef.current = percussionEventsRef.current
        .map(event => ({ ...event, index: event.index - 1 }))
        .filter(event => event.index >= 0);
    }

    // Update chart
    if (chartRef.current) {
      chartRef.current.data.labels = labelsRef.current;
      chartRef.current.data.datasets[0].data = levelDataRef.current;
      chartRef.current.data.datasets[1].data = peakDataRef.current;
      chartRef.current.data.datasets[2].data = valleyDataRef.current;
      chartRef.current.update('none'); // 'none' mode skips animations for performance
    }
  }, [audioData, isStreaming]);

  // Clear data when streaming stops
  const clearData = useCallback(() => {
    levelDataRef.current = [];
    peakDataRef.current = [];
    valleyDataRef.current = [];
    labelsRef.current = [];
    percussionEventsRef.current = [];
    if (chartRef.current) {
      chartRef.current.data.labels = [];
      chartRef.current.data.datasets[0].data = [];
      chartRef.current.data.datasets[1].data = [];
      chartRef.current.data.datasets[2].data = [];
      chartRef.current.update();
    }
  }, []);

  const chartData = {
    labels: labelsRef.current,
    datasets: [
      {
        label: 'Level',
        data: levelDataRef.current,
        borderColor: '#f97316',
        backgroundColor: 'rgba(249, 115, 22, 0.1)',
        borderWidth: 2,
        pointRadius: 0,
        fill: true,
        tension: 0.3,
      },
      {
        label: 'Peak',
        data: peakDataRef.current,
        borderColor: '#3b82f6',
        backgroundColor: 'transparent',
        borderWidth: 1,
        pointRadius: 0,
        borderDash: [5, 5],
        fill: false,
        tension: 0.3,
      },
      {
        label: 'Valley',
        data: valleyDataRef.current,
        borderColor: '#10b981',
        backgroundColor: 'transparent',
        borderWidth: 1,
        pointRadius: 0,
        borderDash: [5, 5],
        fill: false,
        tension: 0.3,
      },
    ],
  };

  const chartOptions = {
    responsive: true,
    maintainAspectRatio: false,
    animation: false as const,
    scales: {
      x: {
        display: false,
      },
      y: {
        min: 0,
        max: 1,
        grid: {
          color: 'rgba(255, 255, 255, 0.1)',
        },
        ticks: {
          color: '#9ca3af',
        },
      },
    },
    plugins: {
      legend: {
        position: 'top' as const,
        labels: {
          color: '#e5e7eb',
          usePointStyle: true,
          padding: 20,
        },
      },
      tooltip: {
        enabled: false,
      },
    },
    interaction: {
      intersect: false,
      mode: 'index' as const,
    },
  };

  // Calculate percussion marker positions
  const getPercussionMarkers = () => {
    if (!chartRef.current || percussionEventsRef.current.length === 0) return null;

    const chart = chartRef.current;
    const xScale = chart.scales.x;
    const yScale = chart.scales.y;

    if (!xScale || !yScale) return null;

    return percussionEventsRef.current.map((event, i) => {
      const xPixel = xScale.getPixelForValue(event.index);
      // Clamp strength to 0-1 range so markers don't render off the chart
      const clampedStrength = Math.min(event.strength, 1.0);
      const yPixel = yScale.getPixelForValue(clampedStrength);
      const bottomPixel = yScale.getPixelForValue(0);

      return (
        <div key={`${event.type}-${event.index}-${i}`} className="percussion-marker">
          <svg
            style={{
              position: 'absolute',
              left: xPixel,
              top: 0,
              width: 2,
              height: '100%',
              pointerEvents: 'none',
            }}
          >
            <line
              x1="0"
              y1={yPixel}
              x2="0"
              y2={bottomPixel}
              stroke={event.color}
              strokeWidth="2"
              opacity="0.8"
            />
          </svg>
          <div
            className="percussion-marker-icon"
            style={{
              position: 'absolute',
              left: xPixel - 12,
              top: yPixel - 12,
              fontSize: '24px',
              pointerEvents: 'none',
              filter: `drop-shadow(0 0 4px ${event.color})`,
            }}
          >
            {event.icon}
          </div>
        </div>
      );
    });
  };

  return (
    <div className="audio-visualizer">
      <div className="audio-header">
        <div>
          <h2>AdaptiveMic Output</h2>
          <span className="audio-header-subtitle">Inputs to Fire Generator</span>
        </div>
        <div className="audio-controls">
          {audioData && isStreaming && (
            <div className="audio-values">
              <span className="audio-value level" title={audioMetricsMetadata['l'].tooltip}>
                {audioMetricsMetadata['l'].displayName}: {audioData.l.toFixed(2)}
              </span>
              <span className="audio-value gain" title={audioMetricsMetadata['pk'].tooltip}>
                {audioMetricsMetadata['pk'].displayName}: {audioData.pk.toFixed(2)}
              </span>
              <span className="audio-value gain" title={audioMetricsMetadata['vl'].tooltip}>
                {audioMetricsMetadata['vl'].displayName}: {audioData.vl.toFixed(2)}
              </span>
              <span className="audio-value gain" title={audioMetricsMetadata['h'].tooltip}>
                {audioMetricsMetadata['h'].displayName}: {audioData.h}
              </span>
            </div>
          )}
          <button className="btn btn-small" onClick={clearData} disabled={disabled || !isStreaming}>
            Clear
          </button>
          <button
            className={`btn btn-small ${isStreaming ? 'btn-danger' : 'btn-primary'}`}
            onClick={onToggleStreaming}
            disabled={disabled}
          >
            {isStreaming ? 'Stop' : 'Start'} Stream
          </button>
        </div>
      </div>

      <div
        className="audio-chart-container"
        ref={chartContainerRef}
        style={{ position: 'relative' }}
      >
        {!isStreaming && !disabled && (
          <div className="audio-placeholder">Click "Start Stream" to visualize audio input</div>
        )}
        {disabled && <div className="audio-placeholder">Connect to device to monitor audio</div>}
        <Line ref={chartRef} data={chartData} options={chartOptions} />
        {getPercussionMarkers()}
      </div>
    </div>
  );
}
