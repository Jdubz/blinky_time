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
import { AudioSample, BatterySample } from '../types';
import { audioMetricsMetadata } from '../data/settingsMetadata';
import { BatteryModal } from './BatteryModal';
import type { BatteryStatusData } from '../services/serial';

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
  batteryData: BatterySample | null;
  batteryStatusData: BatteryStatusData | null;
  isStreaming: boolean;
  onToggleStreaming: () => void;
  onRequestBatteryStatus: () => void;
  disabled: boolean;
}

const MAX_DATA_POINTS = 150; // ~7.5 seconds at 20Hz

export function AudioVisualizer({
  audioData,
  batteryData,
  batteryStatusData,
  isStreaming,
  onToggleStreaming,
  onRequestBatteryStatus,
  disabled,
}: AudioVisualizerProps) {
  const levelDataRef = useRef<number[]>([]);
  const transientDataRef = useRef<number[]>([]);
  const envelopeDataRef = useRef<number[]>([]);
  const labelsRef = useRef<string[]>([]);
  const chartRef = useRef<ChartJS<'line'>>(null);
  const [isDebugModalOpen, setIsDebugModalOpen] = useState(false);

  // Update data when new audio sample arrives
  useEffect(() => {
    if (!audioData || !isStreaming) return;

    // Add new data point
    levelDataRef.current.push(audioData.l);
    transientDataRef.current.push(audioData.t);
    envelopeDataRef.current.push(audioData.e);
    labelsRef.current.push('');

    // Trim to max length
    if (levelDataRef.current.length > MAX_DATA_POINTS) {
      levelDataRef.current.shift();
      transientDataRef.current.shift();
      envelopeDataRef.current.shift();
      labelsRef.current.shift();
    }

    // Update chart
    if (chartRef.current) {
      chartRef.current.data.labels = labelsRef.current;
      chartRef.current.data.datasets[0].data = levelDataRef.current;
      chartRef.current.data.datasets[1].data = transientDataRef.current;
      chartRef.current.data.datasets[2].data = envelopeDataRef.current;
      chartRef.current.update('none'); // 'none' mode skips animations for performance
    }
  }, [audioData, isStreaming]);

  // Clear data when streaming stops
  const clearData = useCallback(() => {
    levelDataRef.current = [];
    transientDataRef.current = [];
    envelopeDataRef.current = [];
    labelsRef.current = [];
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
        label: 'Transient',
        data: transientDataRef.current,
        borderColor: '#ef4444',
        backgroundColor: 'transparent',
        borderWidth: 2,
        pointRadius: 0,
        fill: false,
        tension: 0,
      },
      {
        label: 'Envelope',
        data: envelopeDataRef.current,
        borderColor: '#3b82f6',
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

  return (
    <div className="audio-visualizer">
      <div className="audio-header">
        <h2>Audio Monitor</h2>
        <div className="audio-controls">
          {audioData && isStreaming && (
            <div className="audio-values">
              <span className="audio-value level" title={audioMetricsMetadata['l'].tooltip}>
                {audioMetricsMetadata['l'].displayName}: {audioData.l.toFixed(2)}
              </span>
              <span className="audio-value transient" title={audioMetricsMetadata['t'].tooltip}>
                {audioMetricsMetadata['t'].displayName}: {audioData.t.toFixed(2)}
              </span>
              <span className="audio-value gain" title={audioMetricsMetadata['g'].tooltip}>
                {audioMetricsMetadata['g'].displayName}: {audioData.g.toFixed(1)}x
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

      {batteryData && isStreaming && (
        <div className="battery-status">
          {batteryData.n ? (
            <>
              <div className="battery-indicator">
                <div
                  className="battery-fill"
                  style={{
                    width: `${batteryData.p}%`,
                    backgroundColor:
                      batteryData.p > 50 ? '#22c55e' : batteryData.p > 20 ? '#eab308' : '#ef4444',
                  }}
                />
                {batteryData.c && <span className="battery-charging">⚡</span>}
              </div>
              <span className="battery-text">
                {batteryData.p}% • {batteryData.v.toFixed(2)}V{batteryData.c ? ' • Charging' : ''}
              </span>
              <button
                className="battery-debug-btn"
                onClick={() => setIsDebugModalOpen(true)}
                title="Show battery debug info"
              >
                ℹ️
              </button>
            </>
          ) : (
            <>
              <span className="battery-text" style={{ color: '#6b7280' }}>
                Battery Not Connected
              </span>
              <button
                className="battery-debug-btn"
                onClick={() => setIsDebugModalOpen(true)}
                title="Show battery debug info"
              >
                ℹ️
              </button>
            </>
          )}
        </div>
      )}

      <BatteryModal
        isOpen={isDebugModalOpen}
        onClose={() => setIsDebugModalOpen(false)}
        onRefresh={onRequestBatteryStatus}
        statusData={batteryStatusData}
      />

      <div className="audio-chart-container">
        {!isStreaming && !disabled && (
          <div className="audio-placeholder">Click "Start Stream" to visualize audio input</div>
        )}
        {disabled && <div className="audio-placeholder">Connect to device to monitor audio</div>}
        <Line ref={chartRef} data={chartData} options={chartOptions} />
      </div>
    </div>
  );
}
