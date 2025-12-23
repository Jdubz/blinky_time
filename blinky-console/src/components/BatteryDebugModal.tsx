import { useEffect, useState } from 'react';
import './BatteryDebugModal.css';

interface BatteryDebugData {
  rawCount: number;
  adcBits: number;
  maxCount: number;
  vRef: number;
  vAdc: number;
  dividerRatio: number;
  vBattCalculated: number;
  vBattActual: number;
}

interface BatteryDebugModalProps {
  isOpen: boolean;
  onClose: () => void;
  onRequestDebugData: () => void;
  debugData: BatteryDebugData | null;
}

export function BatteryDebugModal({
  isOpen,
  onClose,
  onRequestDebugData,
  debugData,
}: BatteryDebugModalProps) {
  const [hasRequested, setHasRequested] = useState(false);

  useEffect(() => {
    if (isOpen && !hasRequested) {
      onRequestDebugData();
      setHasRequested(true);
    }
    if (!isOpen) {
      setHasRequested(false);
    }
  }, [isOpen, hasRequested, onRequestDebugData]);

  if (!isOpen) return null;

  return (
    <div className="modal-overlay" onClick={onClose}>
      <div className="modal-content" onClick={e => e.stopPropagation()}>
        <div className="modal-header">
          <h2>Battery Debug Info</h2>
          <button className="modal-close" onClick={onClose}>
            ×
          </button>
        </div>

        <div className="modal-body">
          {debugData ? (
            <div className="debug-data">
              <div className="debug-section">
                <h3>ADC Configuration</h3>
                <div className="debug-row">
                  <span className="debug-label">ADC Bits:</span>
                  <span className="debug-value">{debugData.adcBits}</span>
                </div>
                <div className="debug-row">
                  <span className="debug-label">Max Count:</span>
                  <span className="debug-value">{debugData.maxCount.toFixed(0)}</span>
                </div>
                <div className="debug-row">
                  <span className="debug-label">Reference Voltage:</span>
                  <span className="debug-value">{debugData.vRef.toFixed(3)}V</span>
                </div>
              </div>

              <div className="debug-section">
                <h3>ADC Reading</h3>
                <div className="debug-row">
                  <span className="debug-label">Raw ADC Count:</span>
                  <span className="debug-value mono">{debugData.rawCount}</span>
                </div>
                <div className="debug-row">
                  <span className="debug-label">Pin Voltage (V_adc):</span>
                  <span className="debug-value mono">{debugData.vAdc.toFixed(4)}V</span>
                </div>
              </div>

              <div className="debug-section">
                <h3>Voltage Calculation</h3>
                <div className="debug-row">
                  <span className="debug-label">Divider Ratio:</span>
                  <span className="debug-value mono">{debugData.dividerRatio.toFixed(4)}</span>
                </div>
                <div className="debug-row">
                  <span className="debug-label">Battery (Calculated):</span>
                  <span className="debug-value mono">{debugData.vBattCalculated.toFixed(4)}V</span>
                </div>
                <div className="debug-row">
                  <span className="debug-label">Battery (Actual):</span>
                  <span
                    className={`debug-value mono ${
                      debugData.vBattActual >= 2.5 && debugData.vBattActual <= 4.3
                        ? 'value-valid'
                        : 'value-invalid'
                    }`}
                  >
                    {debugData.vBattActual.toFixed(4)}V
                  </span>
                </div>
              </div>

              <div className="debug-section debug-info">
                <h4>Expected Values</h4>
                <ul>
                  <li>LiPo voltage range: 2.5V - 4.3V</li>
                  <li>Pin voltage should be ~25% of battery voltage</li>
                  <li>For 4.0V battery: Pin should read ~1.01V</li>
                  <li>If values are wrong, check VREF or divider ratio</li>
                </ul>
              </div>
            </div>
          ) : (
            <div className="debug-loading">
              <p>Requesting debug data from device...</p>
              <p className="debug-hint">Make sure streaming is enabled</p>
            </div>
          )}
        </div>

        <div className="modal-footer">
          <button className="btn btn-primary" onClick={onRequestDebugData}>
            Refresh Data
          </button>
          <button className="btn" onClick={onClose}>
            Close
          </button>
        </div>
      </div>
    </div>
  );
}
