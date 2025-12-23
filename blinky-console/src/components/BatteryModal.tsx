import { BatteryStatusData } from '../services/serial';
import './BatteryModal.css';

interface BatteryModalProps {
  isOpen: boolean;
  onClose: () => void;
  statusData: BatteryStatusData | null;
  onRefresh: () => void;
}

export function BatteryModal({ isOpen, onClose, statusData, onRefresh }: BatteryModalProps) {
  if (!isOpen) return null;

  const getBatteryIcon = (percent: number, charging: boolean) => {
    if (charging) return '🔌';
    if (percent >= 75) return '🔋';
    if (percent >= 50) return '🔋';
    if (percent >= 25) return '🪫';
    return '🪫';
  };

  const getBatteryLevelClass = (percent: number) => {
    if (percent >= 75) return 'battery-level-high';
    if (percent >= 50) return 'battery-level-medium';
    if (percent >= 25) return 'battery-level-low';
    return 'battery-level-critical';
  };

  const getBatteryStatus = (percent: number) => {
    if (percent >= 90) return 'Excellent';
    if (percent >= 60) return 'Good';
    if (percent >= 30) return 'Fair';
    if (percent >= 10) return 'Low';
    return 'Critical';
  };

  return (
    <div className="modal-overlay" onClick={onClose}>
      <div className="battery-modal-content" onClick={e => e.stopPropagation()}>
        <div className="modal-header">
          <h2>Battery Status</h2>
          <button className="modal-close" onClick={onClose}>
            ×
          </button>
        </div>

        <div className="battery-body">
          {statusData ? (
            <>
              <div className="battery-main">
                <div className="battery-icon">
                  {getBatteryIcon(statusData.percent, statusData.charging)}
                </div>
                <div className="battery-percent">
                  <span className={getBatteryLevelClass(statusData.percent)}>
                    {statusData.percent}%
                  </span>
                </div>
                <div className="battery-status">{getBatteryStatus(statusData.percent)}</div>
              </div>

              <div className="battery-details">
                <div className="detail-row">
                  <span className="detail-label">Voltage:</span>
                  <span className="detail-value">{statusData.voltage.toFixed(2)}V</span>
                </div>
                <div className="detail-row">
                  <span className="detail-label">Status:</span>
                  <span className="detail-value">
                    {statusData.charging ? (
                      <span className="charging-indicator">⚡ Charging</span>
                    ) : (
                      'On Battery'
                    )}
                  </span>
                </div>
                <div className="detail-row">
                  <span className="detail-label">Connection:</span>
                  <span className="detail-value">
                    {statusData.connected ? (
                      <span className="connected-indicator">✓ Connected</span>
                    ) : (
                      <span className="disconnected-indicator">✗ Not Connected</span>
                    )}
                  </span>
                </div>
              </div>

              {/* Battery health indicator */}
              {statusData.connected && (
                <div className="battery-health">
                  {statusData.voltage < 3.3 && (
                    <div className="health-warning">
                      ⚠️ Low battery! Charge soon to avoid shutdown.
                    </div>
                  )}
                  {statusData.voltage >= 4.15 && !statusData.charging && (
                    <div className="health-good">✓ Battery fully charged</div>
                  )}
                  {statusData.voltage >= 3.3 && statusData.voltage < 3.5 && (
                    <div className="health-warning">Battery low - consider charging</div>
                  )}
                </div>
              )}
            </>
          ) : (
            <div className="battery-loading">
              <p>Loading battery data...</p>
              <button className="btn btn-primary" onClick={onRefresh}>
                Refresh
              </button>
            </div>
          )}
        </div>

        <div className="modal-footer">
          <button className="btn btn-primary" onClick={onRefresh}>
            Refresh
          </button>
          <button className="btn" onClick={onClose}>
            Close
          </button>
        </div>
      </div>
    </div>
  );
}
