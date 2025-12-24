import { useEffect, useState } from 'react';
import { AudioSample } from '../types';
import './PercussionIndicator.css';

interface PercussionIndicatorProps {
  audioData: AudioSample | null;
  isStreaming: boolean;
}

export function PercussionIndicator({ audioData, isStreaming }: PercussionIndicatorProps) {
  const [kickActive, setKickActive] = useState(false);
  const [snareActive, setSnareActive] = useState(false);
  const [hihatActive, setHihatActive] = useState(false);

  // Flash indicators when percussion is detected
  useEffect(() => {
    if (!audioData || !isStreaming) return;

    const timers: number[] = [];

    // Kick detection
    if (audioData.k === 1) {
      setKickActive(true);
      timers.push(setTimeout(() => setKickActive(false), 150)); // Flash for 150ms
    }

    // Snare detection
    if (audioData.sn === 1) {
      setSnareActive(true);
      timers.push(setTimeout(() => setSnareActive(false), 150));
    }

    // Hihat detection
    if (audioData.hh === 1) {
      setHihatActive(true);
      timers.push(setTimeout(() => setHihatActive(false), 150));
    }

    // Cleanup timers on unmount or when dependencies change
    return () => timers.forEach(clearTimeout);
  }, [audioData, isStreaming]);

  if (!isStreaming) {
    return (
      <div className="percussion-indicator-container">
        <div className="percussion-placeholder">Start streaming to see percussion detection</div>
      </div>
    );
  }

  return (
    <div className="percussion-indicator-container">
      <h3 className="percussion-title">Percussion Detection</h3>
      <div className="percussion-indicators">
        <div className={`percussion-item kick ${kickActive ? 'active' : ''}`}>
          <div className="percussion-circle">
            <span className="percussion-icon">🥁</span>
          </div>
          <div className="percussion-label">Kick</div>
          <div className="percussion-strength">
            {audioData && audioData.ks > 0 ? audioData.ks.toFixed(2) : '0.00'}
          </div>
          <div className="percussion-bar">
            <div
              className="percussion-bar-fill kick-fill"
              style={{ width: `${audioData ? audioData.ks * 100 : 0}%` }}
            />
          </div>
        </div>

        <div className={`percussion-item snare ${snareActive ? 'active' : ''}`}>
          <div className="percussion-circle">
            <span className="percussion-icon">🥁</span>
          </div>
          <div className="percussion-label">Snare</div>
          <div className="percussion-strength">
            {audioData && audioData.ss > 0 ? audioData.ss.toFixed(2) : '0.00'}
          </div>
          <div className="percussion-bar">
            <div
              className="percussion-bar-fill snare-fill"
              style={{ width: `${audioData ? audioData.ss * 100 : 0}%` }}
            />
          </div>
        </div>

        <div className={`percussion-item hihat ${hihatActive ? 'active' : ''}`}>
          <div className="percussion-circle">
            <span className="percussion-icon">🎵</span>
          </div>
          <div className="percussion-label">Hi-Hat</div>
          <div className="percussion-strength">
            {audioData && audioData.hs > 0 ? audioData.hs.toFixed(2) : '0.00'}
          </div>
          <div className="percussion-bar">
            <div
              className="percussion-bar-fill hihat-fill"
              style={{ width: `${audioData ? audioData.hs * 100 : 0}%` }}
            />
          </div>
        </div>
      </div>

      {audioData && (
        <div className="percussion-metadata">
          <span
            className="metadata-item"
            title="Zero-Crossing Rate: Higher values indicate more high-frequency content (noise-like sounds)"
          >
            ZCR: {audioData.z.toFixed(2)}
          </span>
        </div>
      )}
    </div>
  );
}
