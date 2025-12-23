import { useEffect, useState } from 'react';
import type { BatteryDebugData } from '../services/serial';
import './BatteryDebugModal.css';

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

  const isValidLiPo = (v: number) => v >= 2.5 && v <= 4.3;

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
              {/* Pin Configuration */}
              <div className="debug-section">
                <h3>Pin Configuration</h3>
                <div className="debug-row">
                  <span className="debug-label">PIN_VBAT:</span>
                  <span className="debug-value mono">{debugData.pins.vbat}</span>
                </div>
                <div className="debug-row">
                  <span className="debug-label">PIN_VBAT_ENABLE:</span>
                  <span className="debug-value mono">{debugData.pins.enable}</span>
                </div>
              </div>

              {/* Sample Readings */}
              <div className="debug-section">
                <h3>Sample Readings (5 consecutive)</h3>
                <div className="debug-row">
                  <span className="debug-label">Samples:</span>
                  <span className="debug-value mono">{debugData.samples.join(', ')}</span>
                </div>
                <div className="debug-row">
                  <span className="debug-label">Min/Max:</span>
                  <span className="debug-value mono">
                    {Math.min(...debugData.samples)} / {Math.max(...debugData.samples)}
                  </span>
                </div>
                <div className="debug-row">
                  <span className="debug-label">Variation:</span>
                  <span className="debug-value mono">
                    {Math.max(...debugData.samples) - Math.min(...debugData.samples) === 0
                      ? 'None (stable)'
                      : `${Math.max(...debugData.samples) - Math.min(...debugData.samples)} counts`}
                  </span>
                </div>
              </div>

              {/* ADC Configuration */}
              <div className="debug-section">
                <h3>ADC Configuration</h3>
                <div className="debug-row">
                  <span className="debug-label">ADC Bits:</span>
                  <span className="debug-value mono">{debugData.adc.bits}</span>
                </div>
                <div className="debug-row">
                  <span className="debug-label">Max Count:</span>
                  <span className="debug-value mono">{debugData.adc.maxCount}</span>
                </div>
                <div className="debug-row">
                  <span className="debug-label">Reference Voltage:</span>
                  <span className="debug-value mono">{debugData.adc.vref.toFixed(3)}V</span>
                </div>
              </div>

              {/* ADC Reading */}
              <div className="debug-section">
                <h3>ADC Reading</h3>
                <div className="debug-row">
                  <span className="debug-label">Raw ADC Count:</span>
                  <span
                    className={`debug-value mono ${
                      debugData.reading.raw >= debugData.adc.maxCount - 10 ? 'value-warning' : ''
                    }`}
                  >
                    {debugData.reading.raw}
                    {debugData.reading.raw >= debugData.adc.maxCount - 10 && ' (MAXED OUT!)'}
                  </span>
                </div>
                <div className="debug-row">
                  <span className="debug-label">Pin Voltage (V_adc):</span>
                  <span
                    className={`debug-value mono ${
                      debugData.reading.vAdc >= debugData.adc.vref - 0.1 ? 'value-warning' : ''
                    }`}
                  >
                    {debugData.reading.vAdc.toFixed(4)}V
                    {debugData.reading.vAdc >= debugData.adc.vref - 0.1 && ' (AT VREF LIMIT!)'}
                  </span>
                </div>
              </div>

              {/* Voltage Calculation */}
              <div className="debug-section">
                <h3>Voltage Calculation</h3>
                <div className="debug-row">
                  <span className="debug-label">Divider Ratio:</span>
                  <span className="debug-value mono">
                    {debugData.reading.dividerRatio.toFixed(4)}
                  </span>
                </div>
                <div className="debug-row">
                  <span className="debug-label">Battery (Calculated):</span>
                  <span
                    className={`debug-value mono ${
                      isValidLiPo(debugData.reading.vBattCalc) ? 'value-valid' : 'value-invalid'
                    }`}
                  >
                    {debugData.reading.vBattCalc.toFixed(4)}V
                  </span>
                </div>
                <div className="debug-row">
                  <span className="debug-label">Battery (Actual):</span>
                  <span
                    className={`debug-value mono ${
                      isValidLiPo(debugData.reading.vBattActual) ? 'value-valid' : 'value-invalid'
                    }`}
                  >
                    {debugData.reading.vBattActual.toFixed(4)}V
                  </span>
                </div>
              </div>

              {/* Platform Info */}
              <div className="debug-section">
                <h3>Platform Info</h3>
                <div className="debug-row">
                  <span className="debug-label">P0_31 defined:</span>
                  <span className="debug-value mono">
                    {debugData.platform.p0_31 ? 'Yes (mbed)' : 'No (non-mbed)'}
                    {debugData.platform.p0_31_value !== undefined &&
                      ` (value: ${debugData.platform.p0_31_value})`}
                  </span>
                </div>
                <div className="debug-row">
                  <span className="debug-label">analogReadResolution():</span>
                  <span className="debug-value mono">
                    {debugData.platform.analogReadRes ? 'Available' : 'NOT available'}
                  </span>
                </div>
                <div className="debug-row">
                  <span className="debug-label">AR_INTERNAL2V4:</span>
                  <span className="debug-value mono">
                    {debugData.platform.ar_internal2v4 ? 'Available' : 'NOT available'}
                  </span>
                </div>
              </div>

              {/* Expected Values & Diagnostics */}
              <div className="debug-section debug-info">
                <h4>Expected Values</h4>
                <ul>
                  <li>LiPo voltage range: 2.5V - 4.3V</li>
                  <li>Pin voltage should be ~25% of battery voltage</li>
                  <li>For 4.0V battery: Pin should read ~1.01V</li>
                  <li>
                    If ADC is maxed out: Voltage divider likely NOT working (enable pin issue or
                    wrong resistors)
                  </li>
                  <li>If samples vary: Unstable ADC readings (timing or noise issue)</li>
                </ul>
              </div>
            </div>
          ) : (
            <div className="debug-loading">
              <p>Requesting debug data from device...</p>
              <p className="debug-hint">Make sure device is connected</p>
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
