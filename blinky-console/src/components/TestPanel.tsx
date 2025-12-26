/**
 * TestPanel - Percussion detection testing interface
 *
 * Uses programmatic test patterns with built-in soft-synths.
 * No file uploads required - patterns are defined in code.
 */

import { useState, useEffect, useRef } from 'react';
import type { GroundTruthHit, DetectionEvent, TypeMetrics, TestPattern } from '../types/testTypes';
import type { PercussionMessage, ConnectionState } from '../types';
import { calculateAllMetrics, exportResultsCSV } from '../lib/testMetrics';
import { PercussionSynth } from '../lib/audioSynth';
import { TEST_PATTERNS } from '../lib/testPatterns';
import './TestPanel.css';

interface TestPanelProps {
  onPercussionEvent: (callback: (msg: PercussionMessage) => void) => () => void;
  connectionState: ConnectionState;
}

export default function TestPanel({ onPercussionEvent, connectionState }: TestPanelProps) {
  const [selectedPattern, setSelectedPattern] = useState<TestPattern | null>(null);
  const [groundTruth, setGroundTruth] = useState<GroundTruthHit[]>([]);
  const [detections, setDetections] = useState<DetectionEvent[]>([]);
  const [isPlaying, setIsPlaying] = useState(false);
  const [testStartTime, setTestStartTime] = useState<number | null>(null);
  const [metrics, setMetrics] = useState<TypeMetrics | null>(null);
  const [progress, setProgress] = useState(0);

  const synthRef = useRef<PercussionSynth | null>(null);
  const timeoutRef = useRef<number | null>(null);
  const progressIntervalRef = useRef<number | null>(null);

  // Initialize synthesizer on mount
  useEffect(() => {
    synthRef.current = new PercussionSynth();

    return () => {
      if (synthRef.current) {
        synthRef.current.dispose();
      }
    };
  }, []);

  // Handle pattern selection
  const handlePatternSelect = (patternId: string) => {
    const pattern = TEST_PATTERNS.find(p => p.id === patternId);
    if (pattern) {
      setSelectedPattern(pattern);
      setGroundTruth(pattern.hits);
      setMetrics(null);
      setDetections([]);
      setProgress(0);
    }
  };

  // Play test pattern
  const playTest = async () => {
    if (connectionState !== 'connected') {
      alert('Device not connected. Please connect device first.');
      return;
    }

    if (!selectedPattern) {
      alert('Please select a test pattern');
      return;
    }

    if (!synthRef.current) {
      alert('Audio synthesizer not initialized');
      return;
    }

    // Resume audio context (required after user interaction)
    await synthRef.current.resume();

    // Reset state
    setDetections([]);
    setMetrics(null);
    setProgress(0);
    setIsPlaying(true);
    setTestStartTime(Date.now());

    // Schedule all percussion hits
    const synth = synthRef.current;

    for (const hit of selectedPattern.hits) {
      synth.trigger(hit.type, hit.time, hit.strength);
    }

    // Update progress based on elapsed time
    const durationMs = selectedPattern.durationMs;
    const actualStartTime = Date.now();

    const progressTimer = setInterval(() => {
      const elapsed = Date.now() - actualStartTime;
      setProgress(Math.min((elapsed / durationMs) * 100, 100));
    }, 100);
    progressIntervalRef.current = progressTimer;

    // Stop test when pattern completes
    const testTimeout = setTimeout(() => {
      clearInterval(progressTimer);
      setIsPlaying(false);
      setProgress(100);

      // Wait for final serial messages to arrive
      setTimeout(() => {
        setDetections(current => {
          const finalMetrics = calculateAllMetrics(groundTruth, current);
          setMetrics(finalMetrics);
          return current;
        });
      }, 500);
    }, durationMs + 100);

    timeoutRef.current = testTimeout;
  };

  // Stop test
  const stopTest = () => {
    if (timeoutRef.current) {
      clearTimeout(timeoutRef.current);
    }
    if (progressIntervalRef.current) {
      clearInterval(progressIntervalRef.current);
    }
    setIsPlaying(false);
    setProgress(0);
  };

  // Listen for percussion events from device
  useEffect(() => {
    if (!isPlaying || !testStartTime) return;

    const handlePercussion = (msg: PercussionMessage) => {
      const elapsedMs = Date.now() - testStartTime;

      // Record detections
      const newDetections: DetectionEvent[] = [];

      if (msg.kick) {
        newDetections.push({
          timestampMs: elapsedMs,
          type: 'kick',
          strength: msg.kickStrength || 0,
        });
      }
      if (msg.snare) {
        newDetections.push({
          timestampMs: elapsedMs,
          type: 'snare',
          strength: msg.snareStrength || 0,
        });
      }
      if (msg.hihat) {
        newDetections.push({
          timestampMs: elapsedMs,
          type: 'hihat',
          strength: msg.hihatStrength || 0,
        });
      }

      if (newDetections.length > 0) {
        setDetections(prev => {
          const updated = [...prev, ...newDetections];
          // Calculate live metrics with updated detections
          const liveMetrics = calculateAllMetrics(groundTruth, updated);
          setMetrics(liveMetrics);
          return updated;
        });
      }
    };

    // Register percussion event listener
    const cleanup = onPercussionEvent(handlePercussion);

    return cleanup;
  }, [isPlaying, testStartTime, groundTruth, onPercussionEvent]);

  // Export results
  const exportResults = () => {
    if (!metrics || !selectedPattern) return;

    const csv = exportResultsCSV(selectedPattern.name, metrics);

    const blob = new Blob([csv], { type: 'text/csv' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `test-results-${selectedPattern.id}-${Date.now()}.csv`;
    a.click();
    URL.revokeObjectURL(url);
  };

  return (
    <div className="test-panel">
      <h2>Percussion Detection Test</h2>

      {/* Pattern Selection */}
      <div className="pattern-section">
        <label htmlFor="pattern-select">Test Pattern:</label>
        <select
          id="pattern-select"
          value={selectedPattern?.id || ''}
          onChange={e => handlePatternSelect(e.target.value)}
          disabled={isPlaying}
        >
          <option value="">Select a pattern...</option>
          {TEST_PATTERNS.map(pattern => (
            <option key={pattern.id} value={pattern.id}>
              {pattern.name}
            </option>
          ))}
        </select>

        {selectedPattern && (
          <div className="pattern-info">
            <p className="pattern-description">{selectedPattern.description}</p>
            <div className="pattern-stats">
              <span>Duration: {(selectedPattern.durationMs / 1000).toFixed(1)}s</span>
              {selectedPattern.bpm && <span>BPM: {selectedPattern.bpm}</span>}
              <span>Hits: {selectedPattern.hits.length}</span>
            </div>
          </div>
        )}
      </div>

      {/* Control Buttons */}
      <div className="controls">
        <button className="play-button" onClick={playTest} disabled={!selectedPattern || isPlaying}>
          ▶ Play Test
        </button>
        <button className="stop-button" onClick={stopTest} disabled={!isPlaying}>
          ⏹ Stop
        </button>
      </div>

      {/* Progress Bar */}
      {isPlaying && (
        <div className="progress-section">
          <div className="progress-bar">
            <div className="progress-fill" style={{ width: `${progress}%` }} />
          </div>
          <div className="progress-text">
            {progress.toFixed(0)}% ({detections.length} detections)
          </div>
        </div>
      )}

      {/* Metrics Display */}
      {metrics && (
        <div className="metrics-section">
          <h3>Test Results</h3>

          {/* Overall Metrics */}
          <div className="metrics-overall">
            <div className="metric-card">
              <div className="metric-label">F1 Score</div>
              <div className="metric-value large">
                {(metrics.overall.f1Score * 100).toFixed(1)}%
              </div>
            </div>
            <div className="metric-card">
              <div className="metric-label">Precision</div>
              <div className="metric-value">{(metrics.overall.precision * 100).toFixed(1)}%</div>
            </div>
            <div className="metric-card">
              <div className="metric-label">Recall</div>
              <div className="metric-value">{(metrics.overall.recall * 100).toFixed(1)}%</div>
            </div>
          </div>

          {/* Detailed Metrics */}
          <div className="metrics-detail">
            <div className="metric-row">
              <span>True Positives:</span>
              <span className="metric-count">{metrics.overall.truePositives}</span>
            </div>
            <div className="metric-row">
              <span>False Positives:</span>
              <span className="metric-count error">{metrics.overall.falsePositives}</span>
            </div>
            <div className="metric-row">
              <span>False Negatives:</span>
              <span className="metric-count error">{metrics.overall.falseNegatives}</span>
            </div>
            {metrics.overall.avgTimingErrorMs !== undefined && (
              <div className="metric-row">
                <span>Avg Timing Error:</span>
                <span className="metric-count">
                  {metrics.overall.avgTimingErrorMs.toFixed(1)}ms
                </span>
              </div>
            )}
          </div>

          {/* Per-Type Breakdown */}
          <div className="metrics-breakdown">
            <h4>Per-Type Metrics</h4>
            <table>
              <thead>
                <tr>
                  <th>Type</th>
                  <th>F1</th>
                  <th>Precision</th>
                  <th>Recall</th>
                  <th>TP</th>
                  <th>FP</th>
                  <th>FN</th>
                </tr>
              </thead>
              <tbody>
                <tr>
                  <td>Kick</td>
                  <td>{(metrics.kick.f1Score * 100).toFixed(1)}%</td>
                  <td>{(metrics.kick.precision * 100).toFixed(1)}%</td>
                  <td>{(metrics.kick.recall * 100).toFixed(1)}%</td>
                  <td>{metrics.kick.truePositives}</td>
                  <td>{metrics.kick.falsePositives}</td>
                  <td>{metrics.kick.falseNegatives}</td>
                </tr>
                <tr>
                  <td>Snare</td>
                  <td>{(metrics.snare.f1Score * 100).toFixed(1)}%</td>
                  <td>{(metrics.snare.precision * 100).toFixed(1)}%</td>
                  <td>{(metrics.snare.recall * 100).toFixed(1)}%</td>
                  <td>{metrics.snare.truePositives}</td>
                  <td>{metrics.snare.falsePositives}</td>
                  <td>{metrics.snare.falseNegatives}</td>
                </tr>
                <tr>
                  <td>Hihat</td>
                  <td>{(metrics.hihat.f1Score * 100).toFixed(1)}%</td>
                  <td>{(metrics.hihat.precision * 100).toFixed(1)}%</td>
                  <td>{(metrics.hihat.recall * 100).toFixed(1)}%</td>
                  <td>{metrics.hihat.truePositives}</td>
                  <td>{metrics.hihat.falsePositives}</td>
                  <td>{metrics.hihat.falseNegatives}</td>
                </tr>
              </tbody>
            </table>
          </div>

          {/* Export Button */}
          <button className="export-button" onClick={exportResults}>
            Export Results CSV
          </button>
        </div>
      )}

      {/* Instructions */}
      {!selectedPattern && (
        <div className="instructions">
          <h3>How to Use</h3>
          <ol>
            <li>Select a test pattern from the dropdown</li>
            <li>Position the device near your speaker</li>
            <li>Click "Play Test" - synthesized percussion will play</li>
            <li>Watch metrics update live as device detects percussion</li>
            <li>Adjust detection parameters in Settings tab</li>
            <li>Re-run tests to see improvements</li>
          </ol>
          <div className="pattern-list">
            <h4>Available Patterns:</h4>
            <ul>
              {TEST_PATTERNS.map(pattern => (
                <li key={pattern.id}>
                  <strong>{pattern.name}</strong>: {pattern.description}
                </li>
              ))}
            </ul>
          </div>
        </div>
      )}
    </div>
  );
}
