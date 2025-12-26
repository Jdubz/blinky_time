/**
 * TestPanel - Percussion detection testing interface
 *
 * Allows loading test audio + ground truth, playing tests,
 * and viewing real-time accuracy metrics.
 */

import { useState, useEffect, useRef } from 'react';
import type { GroundTruthHit, DetectionEvent, TypeMetrics } from '../types/testTypes';
import type { PercussionMessage, ConnectionState } from '../types';
import { parseGroundTruthCSV, calculateAllMetrics, exportResultsCSV } from '../lib/testMetrics';
import './TestPanel.css';

interface TestPanelProps {
  onPercussionEvent: (callback: (msg: PercussionMessage) => void) => () => void;
  connectionState: ConnectionState;
}

export default function TestPanel({ onPercussionEvent, connectionState }: TestPanelProps) {
  const [audioFile, setAudioFile] = useState<File | null>(null);
  const [csvFile, setCsvFile] = useState<File | null>(null);
  const [groundTruth, setGroundTruth] = useState<GroundTruthHit[]>([]);
  const [detections, setDetections] = useState<DetectionEvent[]>([]);
  const [isPlaying, setIsPlaying] = useState(false);
  const [testStartTime, setTestStartTime] = useState<number | null>(null);
  const [metrics, setMetrics] = useState<TypeMetrics | null>(null);
  const [progress, setProgress] = useState(0);

  const audioRef = useRef<HTMLAudioElement | null>(null);

  // Load CSV ground truth
  const handleCsvUpload = async (file: File) => {
    setCsvFile(file);
    try {
      const gt = await parseGroundTruthCSV(file);
      setGroundTruth(gt);
      console.log(`Loaded ${gt.length} ground truth annotations`);
    } catch (error) {
      console.error('Failed to parse CSV:', error);
      alert('Failed to parse CSV file. Check format.');
    }
  };

  // Load audio file
  const handleAudioUpload = (file: File) => {
    setAudioFile(file);
  };

  // Play test
  const playTest = () => {
    if (connectionState !== 'connected') {
      alert('Device not connected. Please connect device first.');
      return;
    }

    if (!audioFile || groundTruth.length === 0) {
      alert('Please load both audio file and CSV annotations');
      return;
    }

    // Reset state
    setDetections([]);
    setMetrics(null);
    setProgress(0);
    setIsPlaying(true);
    setTestStartTime(Date.now());

    // Create and play audio (Fix BUG #9: Track URL for cleanup)
    const audioUrl = URL.createObjectURL(audioFile);
    const audio = new Audio(audioUrl);
    audioRef.current = audio;

    audio.play();

    // Update progress (Fix BUG #10: Check for zero duration)
    const progressInterval = setInterval(() => {
      if (audio.currentTime && audio.duration && audio.duration > 0) {
        setProgress((audio.currentTime / audio.duration) * 100);
      }
    }, 100);

    audio.onended = () => {
      clearInterval(progressInterval);
      setIsPlaying(false);
      setProgress(100);

      // Fix BUG #5: Wait for final serial messages to arrive
      setTimeout(() => {
        setDetections(current => {
          const finalMetrics = calculateAllMetrics(groundTruth, current);
          setMetrics(finalMetrics);
          console.log('Test complete. Metrics:', finalMetrics);
          return current;
        });
      }, 500);

      // Clean up audio URL
      URL.revokeObjectURL(audioUrl);
    };
  };

  // Stop test
  const stopTest = () => {
    if (audioRef.current) {
      audioRef.current.pause();
      audioRef.current.currentTime = 0;
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
        // Fix BUG #4: Race condition - use functional state update
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
    if (!metrics || !audioFile) return;

    const csv = exportResultsCSV(audioFile.name, metrics);

    const blob = new Blob([csv], { type: 'text/csv' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `test-results-${Date.now()}.csv`;
    a.click();
    URL.revokeObjectURL(url);
  };

  return (
    <div className="test-panel">
      <h2>Percussion Detection Test</h2>

      {/* File Upload Section */}
      <div className="upload-section">
        <div className="file-input">
          <label htmlFor="audio-file">Audio File:</label>
          <input
            id="audio-file"
            type="file"
            accept=".wav,.mp3"
            onChange={e => e.target.files?.[0] && handleAudioUpload(e.target.files[0])}
          />
          {audioFile && <span className="file-name">✓ {audioFile.name}</span>}
        </div>

        <div className="file-input">
          <label htmlFor="csv-file">Ground Truth CSV:</label>
          <input
            id="csv-file"
            type="file"
            accept=".csv"
            onChange={e => e.target.files?.[0] && handleCsvUpload(e.target.files[0])}
          />
          {csvFile && <span className="file-name">✓ {groundTruth.length} annotations</span>}
        </div>
      </div>

      {/* Control Buttons */}
      <div className="controls">
        <button
          className="play-button"
          onClick={playTest}
          disabled={!audioFile || groundTruth.length === 0 || isPlaying}
        >
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
      {!audioFile && !csvFile && (
        <div className="instructions">
          <h3>How to Use</h3>
          <ol>
            <li>Load a test audio file (WAV or MP3)</li>
            <li>Load the corresponding ground truth CSV file</li>
            <li>Position the device near your speaker</li>
            <li>Click "Play Test" and watch the metrics update live</li>
            <li>Adjust detection parameters in Settings tab</li>
            <li>Re-run tests to see improvements</li>
          </ol>
        </div>
      )}
    </div>
  );
}
