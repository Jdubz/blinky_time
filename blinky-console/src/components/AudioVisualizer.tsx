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
import {
  AudioSample,
  TransientMessage,
  ConnectionState,
  RhythmData,
  MusicModeData,
  StatusMessage,
} from '../types';
import { audioMetricsMetadata } from '../data/settingsMetadata';
import type {
  GroundTruthHit,
  DetectionEvent,
  TypeMetrics,
  TestPattern,
  TransientType,
} from '../types/testTypes';
import { calculateAllMetrics, exportResultsCSV } from '../lib/testMetrics';
import { PercussionSynth } from '../lib/audioSynth';
import { TEST_PATTERNS } from '../lib/testPatterns';

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

// Test timing constants
const TEST_COMPLETION_BUFFER_MS = 100;
const FINAL_METRICS_DELAY_MS = 500;

interface AudioVisualizerProps {
  audioData: AudioSample | null;
  rhythmData?: RhythmData | null;
  musicModeData?: MusicModeData | null;
  statusData?: StatusMessage | null;
  isStreaming: boolean;
  onToggleStreaming: () => void;
  disabled: boolean;
  // Test mode props
  onPercussionEvent?: (callback: (msg: TransientMessage) => void) => () => void;
  connectionState?: ConnectionState;
}

interface TransientEvent {
  index: number;
  type: TransientType;
  strength: number;
  icon: string;
  color: string;
}

interface GroundTruthMarker {
  index: number;
  type: TransientType;
  strength: number;
  matched: boolean;
}

const MAX_DATA_POINTS = 150; // ~7.5 seconds at 20Hz
const SAMPLES_PER_SECOND = 20; // Approximate sample rate for timing calculations

export function AudioVisualizer({
  audioData,
  rhythmData,
  musicModeData,
  statusData,
  isStreaming,
  onToggleStreaming,
  disabled,
  onPercussionEvent,
  connectionState,
}: AudioVisualizerProps) {
  const levelDataRef = useRef<number[]>([]);
  const peakDataRef = useRef<number[]>([]);
  const valleyDataRef = useRef<number[]>([]);
  const labelsRef = useRef<string[]>([]);
  const chartRef = useRef<ChartJS<'line'>>(null);
  const transientEventsRef = useRef<TransientEvent[]>([]);
  const beatEventsRef = useRef<
    { index: number; type: 'quarter' | 'half' | 'whole'; icon: string; color: string }[]
  >([]);
  const chartContainerRef = useRef<HTMLDivElement>(null);
  const [, setRenderTrigger] = useState(0);

  // Test mode state
  const [selectedPattern, setSelectedPattern] = useState<TestPattern | null>(null);
  const [isTestPlaying, setIsTestPlaying] = useState(false);
  const [testStartTime, setTestStartTime] = useState<number | null>(null);
  const [, setTestDetections] = useState<DetectionEvent[]>([]);
  const [testMetrics, setTestMetrics] = useState<TypeMetrics | null>(null);
  const [testProgress, setTestProgress] = useState(0);
  const groundTruthMarkersRef = useRef<GroundTruthMarker[]>([]);
  const synthRef = useRef<PercussionSynth | null>(null);
  const testTimeoutRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const progressIntervalRef = useRef<ReturnType<typeof setInterval> | null>(null);
  const groundTruthRef = useRef<GroundTruthHit[]>([]);
  const testStartIndexRef = useRef<number>(0);

  // Update data when new audio sample arrives
  useEffect(() => {
    if (!audioData || !isStreaming) return;

    const currentIndex = levelDataRef.current.length;

    // Track transient events (simplified single-band amplitude spike detection)
    if (audioData.t > 0) {
      transientEventsRef.current.push({
        index: currentIndex,
        type: 'transient',
        strength: audioData.t,
        icon: '💥',
        color: '#f59e0b', // Amber for transients
      });
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

      // Shift transient events and remove those that are off the chart
      transientEventsRef.current = transientEventsRef.current
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

  // Track beat events from music mode
  useEffect(() => {
    if (!musicModeData || !isStreaming || !musicModeData.a) return;

    const currentIndex = levelDataRef.current.length - 1; // Current data point
    if (currentIndex < 0) return;

    // Check for beat events
    if (musicModeData.w === 1) {
      beatEventsRef.current.push({
        index: currentIndex,
        type: 'whole',
        icon: '𝅝',
        color: '#8b5cf6', // Purple for whole notes
      });
      setRenderTrigger(n => n + 1);
    } else if (musicModeData.h === 1) {
      beatEventsRef.current.push({
        index: currentIndex,
        type: 'half',
        icon: '♪',
        color: '#3b82f6', // Blue for half notes
      });
      setRenderTrigger(n => n + 1);
    } else if (musicModeData.q === 1) {
      beatEventsRef.current.push({
        index: currentIndex,
        type: 'quarter',
        icon: '♩',
        color: '#10b981', // Green for quarter notes
      });
      setRenderTrigger(n => n + 1);
    }

    // Shift beat events when data shifts
    beatEventsRef.current = beatEventsRef.current
      .map(event => ({ ...event, index: event.index }))
      .filter(event => event.index >= 0 && event.index < MAX_DATA_POINTS);
  }, [musicModeData, isStreaming]);

  // Initialize synthesizer for test mode
  useEffect(() => {
    synthRef.current = new PercussionSynth();

    return () => {
      if (testTimeoutRef.current) clearTimeout(testTimeoutRef.current);
      if (progressIntervalRef.current) clearInterval(progressIntervalRef.current);
      if (synthRef.current) {
        synthRef.current.stop();
        synthRef.current.dispose();
      }
    };
  }, []);

  // Listen for transient events during test
  useEffect(() => {
    if (!isTestPlaying || !testStartTime || !onPercussionEvent) return;

    const handleTransient = (msg: TransientMessage) => {
      const elapsedMs = Date.now() - testStartTime;
      const newDetections: DetectionEvent[] = [];

      // Simplified single-band detection
      newDetections.push({
        timestampMs: elapsedMs,
        type: 'transient',
        strength: msg.strength,
      });

      if (newDetections.length > 0) {
        setTestDetections(prev => {
          const updated = [...prev, ...newDetections];
          const liveMetrics = calculateAllMetrics(groundTruthRef.current, updated);
          setTestMetrics(liveMetrics);
          return updated;
        });
      }
    };

    const cleanup = onPercussionEvent(handleTransient);
    return cleanup;
  }, [isTestPlaying, testStartTime, onPercussionEvent]);

  // Handle pattern selection
  const handlePatternSelect = (patternId: string) => {
    const pattern = TEST_PATTERNS.find(p => p.id === patternId);
    if (pattern) {
      setSelectedPattern(pattern);
      groundTruthRef.current = pattern.hits;
      setTestMetrics(null);
      setTestDetections([]);
      setTestProgress(0);
      groundTruthMarkersRef.current = [];
    }
  };

  // Play test
  const playTest = async () => {
    if (connectionState !== 'connected' || !selectedPattern || !synthRef.current) return;

    try {
      await synthRef.current.resume();
    } catch (error) {
      alert('Failed to start audio. Please check browser permissions.');
      console.error('AudioContext resume failed:', error);
      return;
    }

    // Clear previous data and reset state
    clearData();
    setTestDetections([]);
    setTestMetrics(null);
    setTestProgress(0);
    setIsTestPlaying(true);
    setTestStartTime(Date.now());
    testStartIndexRef.current = levelDataRef.current.length;

    // Pre-calculate ground truth marker positions
    groundTruthMarkersRef.current = selectedPattern.hits.map(hit => ({
      index: Math.round(hit.time * SAMPLES_PER_SECOND),
      type: hit.type,
      strength: hit.strength,
      matched: false,
    }));

    // Schedule all percussion hits
    const synth = synthRef.current;
    synth.start();
    const schedulingStartTime = synth.getCurrentTime();
    for (const hit of selectedPattern.hits) {
      synth.trigger(hit.type, schedulingStartTime + hit.time, hit.strength);
    }

    // Update progress
    const durationMs = selectedPattern.durationMs;
    const actualStartTime = Date.now();

    const progressTimer = setInterval(() => {
      const elapsed = Date.now() - actualStartTime;
      setTestProgress(Math.min((elapsed / durationMs) * 100, 100));
    }, 100);
    progressIntervalRef.current = progressTimer;

    // Stop test when pattern completes
    const testTimeout = setTimeout(() => {
      clearInterval(progressTimer);
      setIsTestPlaying(false);
      setTestProgress(100);

      setTimeout(() => {
        setTestDetections(current => {
          const finalMetrics = calculateAllMetrics(groundTruthRef.current, current);
          setTestMetrics(finalMetrics);
          return current;
        });
      }, FINAL_METRICS_DELAY_MS);
    }, durationMs + TEST_COMPLETION_BUFFER_MS);

    testTimeoutRef.current = testTimeout;
  };

  // Stop test
  const stopTest = () => {
    if (synthRef.current) synthRef.current.stop();
    if (testTimeoutRef.current) clearTimeout(testTimeoutRef.current);
    if (progressIntervalRef.current) clearInterval(progressIntervalRef.current);
    setIsTestPlaying(false);
    setTestProgress(0);
  };

  // Export results
  const exportResults = () => {
    if (!testMetrics || !selectedPattern) return;
    const csv = exportResultsCSV(selectedPattern.name, testMetrics);
    const blob = new Blob([csv], { type: 'text/csv' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `test-results-${selectedPattern.id}-${Date.now()}.csv`;
    a.click();
    URL.revokeObjectURL(url);
  };

  // Clear data when streaming stops
  const clearData = useCallback(() => {
    levelDataRef.current = [];
    peakDataRef.current = [];
    valleyDataRef.current = [];
    labelsRef.current = [];
    transientEventsRef.current = [];
    beatEventsRef.current = [];
    groundTruthMarkersRef.current = [];
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

  // Calculate transient marker positions
  const getTransientMarkers = () => {
    if (!chartRef.current || transientEventsRef.current.length === 0) return null;

    const chart = chartRef.current;
    const xScale = chart.scales.x;
    const yScale = chart.scales.y;

    if (!xScale || !yScale) return null;

    return transientEventsRef.current.map((event, i) => {
      const xPixel = xScale.getPixelForValue(event.index);
      // Clamp strength to 0-1 range so markers don't render off the chart
      const clampedStrength = Math.min(event.strength, 1.0);
      const yPixel = yScale.getPixelForValue(clampedStrength);
      const bottomPixel = yScale.getPixelForValue(0);

      return (
        <div key={`${event.type}-${event.index}-${i}`} className="transient-marker">
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
            className="transient-marker-icon"
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

  // Calculate beat event marker positions (musical notes at top)
  const getBeatMarkers = () => {
    if (!chartRef.current || beatEventsRef.current.length === 0) return null;

    const chart = chartRef.current;
    const xScale = chart.scales.x;
    const yScale = chart.scales.y;

    if (!xScale || !yScale) return null;

    const topPixel = yScale.getPixelForValue(1);

    return beatEventsRef.current.map((event, i) => {
      const xPixel = xScale.getPixelForValue(event.index);

      return (
        <div key={`beat-${event.type}-${event.index}-${i}`} className="beat-marker">
          <div
            className="beat-marker-icon"
            style={{
              position: 'absolute',
              left: xPixel - 10,
              top: topPixel - 30,
              fontSize: '20px',
              pointerEvents: 'none',
              color: event.color,
              textShadow: `0 0 4px ${event.color}`,
              fontWeight: 'bold',
            }}
          >
            {event.icon}
          </div>
        </div>
      );
    });
  };

  // Calculate ground truth marker positions (triangles at top, pointing down)
  const getGroundTruthMarkers = () => {
    if (!chartRef.current || !isTestPlaying || groundTruthMarkersRef.current.length === 0)
      return null;

    const chart = chartRef.current;
    const xScale = chart.scales.x;
    const yScale = chart.scales.y;

    if (!xScale || !yScale) return null;

    const topPixel = yScale.getPixelForValue(1);
    const typeColors: Record<TransientType, string> = {
      transient: '#f59e0b', // Amber for transients
    };

    return groundTruthMarkersRef.current
      .filter(marker => marker.index >= 0 && marker.index < levelDataRef.current.length)
      .map((marker, i) => {
        const xPixel = xScale.getPixelForValue(marker.index);
        const color = typeColors[marker.type];

        return (
          <div key={`gt-${marker.type}-${marker.index}-${i}`} className="ground-truth-marker">
            <svg
              style={{
                position: 'absolute',
                left: xPixel - 6,
                top: topPixel - 2,
                width: 12,
                height: 12,
                pointerEvents: 'none',
              }}
            >
              <polygon points="6,12 0,0 12,0" fill={color} opacity="0.6" />
            </svg>
          </div>
        );
      });
  };

  // Determine if test mode is available
  const testModeAvailable = onPercussionEvent !== undefined;

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

      {/* Test Controls */}
      {testModeAvailable && (
        <div className="test-controls">
          <select
            className="test-pattern-select"
            value={selectedPattern?.id || ''}
            onChange={e => handlePatternSelect(e.target.value)}
            disabled={isTestPlaying || disabled}
          >
            <option value="">Select test pattern...</option>
            {TEST_PATTERNS.map(pattern => (
              <option key={pattern.id} value={pattern.id}>
                {pattern.name}
              </option>
            ))}
          </select>
          <button
            className="btn btn-small btn-success"
            onClick={playTest}
            disabled={!selectedPattern || isTestPlaying || connectionState !== 'connected'}
          >
            {isTestPlaying ? `Testing... ${testProgress.toFixed(0)}%` : 'Run Test'}
          </button>
          {isTestPlaying && (
            <button className="btn btn-small btn-danger" onClick={stopTest}>
              Stop
            </button>
          )}
          {testMetrics && !isTestPlaying && (
            <button className="btn btn-small" onClick={exportResults}>
              Export CSV
            </button>
          )}
        </div>
      )}

      {/* Progress Bar during test */}
      {isTestPlaying && (
        <div className="test-progress-bar">
          <div className="test-progress-fill" style={{ width: `${testProgress}%` }} />
        </div>
      )}

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
        {getTransientMarkers()}
        {getBeatMarkers()}
        {getGroundTruthMarkers()}
      </div>

      {/* Rhythm & Music Mode Indicators */}
      {isStreaming && (rhythmData || musicModeData || statusData) && (
        <div className="telemetry-indicators">
          {statusData && (
            <div className="telemetry-group">
              <span className="telemetry-label">Mode</span>
              <span className="telemetry-value">
                {['Drummer', 'Bass', 'HFC', 'Flux', 'Hybrid'][statusData.mode] || statusData.mode}
              </span>
            </div>
          )}
          {rhythmData && (
            <>
              <div className="telemetry-group">
                <span className="telemetry-label">BPM</span>
                <span className="telemetry-value">{rhythmData.bpm.toFixed(1)}</span>
              </div>
              <div className="telemetry-group">
                <span className="telemetry-label">Periodicity</span>
                <span
                  className={`telemetry-value ${rhythmData.str > 0.7 ? 'good' : rhythmData.str > 0.4 ? 'warn' : ''}`}
                >
                  {(rhythmData.str * 100).toFixed(0)}%
                </span>
              </div>
              <div className="telemetry-group">
                <span className="telemetry-label">Phase</span>
                <span className="telemetry-value">{rhythmData.ph.toFixed(2)}</span>
              </div>
              <div className="telemetry-group">
                <span className="telemetry-label">Buffer</span>
                <span className="telemetry-value">{rhythmData.buf}/256</span>
              </div>
            </>
          )}
          {musicModeData && (
            <>
              <div className="telemetry-group">
                <span className="telemetry-label">Music</span>
                <span className={`telemetry-value ${musicModeData.a ? 'good' : ''}`}>
                  {musicModeData.a ? 'ACTIVE' : 'Inactive'}
                </span>
              </div>
              {musicModeData.a === 1 && (
                <>
                  <div className="telemetry-group">
                    <span className="telemetry-label">Confidence</span>
                    <span
                      className={`telemetry-value ${musicModeData.conf > 0.7 ? 'good' : musicModeData.conf > 0.4 ? 'warn' : ''}`}
                    >
                      {(musicModeData.conf * 100).toFixed(0)}%
                    </span>
                  </div>
                  <div className="telemetry-group">
                    <span className="telemetry-label">Beats</span>
                    <span className="telemetry-value">
                      {musicModeData.q ? '♩' : ''}
                      {musicModeData.h ? '♪' : ''}
                      {musicModeData.w ? '𝅝' : ''}
                      {!musicModeData.q && !musicModeData.h && !musicModeData.w ? '—' : ''}
                    </span>
                  </div>
                </>
              )}
            </>
          )}
        </div>
      )}

      {/* Compact Metrics Panel */}
      {testMetrics && (
        <div className="test-metrics-compact">
          <div className="test-metrics-row">
            <div className="test-metric">
              <span className="test-metric-label">F1</span>
              <span
                className={`test-metric-value ${testMetrics.overall.f1Score > 0.5 ? 'good' : testMetrics.overall.f1Score > 0.2 ? 'warn' : 'bad'}`}
              >
                {(testMetrics.overall.f1Score * 100).toFixed(1)}%
              </span>
            </div>
            <div className="test-metric">
              <span className="test-metric-label">Precision</span>
              <span className="test-metric-value">
                {(testMetrics.overall.precision * 100).toFixed(1)}%
              </span>
            </div>
            <div className="test-metric">
              <span className="test-metric-label">Recall</span>
              <span className="test-metric-value">
                {(testMetrics.overall.recall * 100).toFixed(1)}%
              </span>
            </div>
            <div className="test-metric">
              <span className="test-metric-label">TP</span>
              <span className="test-metric-value good">{testMetrics.overall.truePositives}</span>
            </div>
            <div className="test-metric">
              <span className="test-metric-label">FP</span>
              <span className="test-metric-value bad">{testMetrics.overall.falsePositives}</span>
            </div>
            <div className="test-metric">
              <span className="test-metric-label">FN</span>
              <span className="test-metric-value bad">{testMetrics.overall.falseNegatives}</span>
            </div>
            {testMetrics.overall.avgTimingErrorMs !== undefined && (
              <div className="test-metric">
                <span className="test-metric-label">Timing</span>
                <span className="test-metric-value">
                  {testMetrics.overall.avgTimingErrorMs.toFixed(1)}ms
                </span>
              </div>
            )}
          </div>
        </div>
      )}
    </div>
  );
}
