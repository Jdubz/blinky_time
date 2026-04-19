import { useState } from 'react';
import { DeviceInfo, ConnectionState, BatterySample, isDeviceConfigured } from '../types';
import { BatteryModal } from './BatteryModal';
import type { BatteryStatusData } from '../services/serial';

interface ConnectionBarProps {
  connectionState: ConnectionState;
  deviceInfo: DeviceInfo | null;
  batteryData: BatterySample | null;
  batteryStatusData: BatteryStatusData | null;
  isSupported: boolean;
  errorMessage: string | null;
  onConnect: () => void;
  onDisconnect: () => void;
  onOpenConsole: () => void;
  onRequestBatteryStatus: () => void;
  onBackToList?: () => void;
}

export function ConnectionBar({
  connectionState,
  deviceInfo,
  batteryData,
  batteryStatusData,
  isSupported,
  errorMessage,
  onConnect,
  onDisconnect,
  onOpenConsole,
  onRequestBatteryStatus,
  onBackToList,
}: ConnectionBarProps) {
  const [isBatteryModalOpen, setIsBatteryModalOpen] = useState(false);
  const getStatusColor = () => {
    switch (connectionState) {
      case 'connected':
        return '#4ade80';
      case 'connecting':
        return '#facc15';
      case 'error':
        return '#f87171';
      default:
        return '#6b7280';
    }
  };

  const getStatusText = () => {
    switch (connectionState) {
      case 'connected':
        return 'Connected';
      case 'connecting':
        return 'Connecting...';
      case 'error':
        return errorMessage ? `Error: ${errorMessage}` : 'Connection Error';
      default:
        return 'Disconnected';
    }
  };

  if (!isSupported) {
    return (
      <div className="connection-bar error">
        <span className="warning-icon">&#9888;</span>
        <span>WebSerial not supported. Please use Chrome, Edge, or Opera.</span>
      </div>
    );
  }

  return (
    <>
      <div className="connection-bar">
        <div className="connection-left">
          {onBackToList && (
            <button className="btn btn-back" onClick={onBackToList} title="Back to devices">
              &larr;
            </button>
          )}
          <span className="app-title">Blinky Console</span>
          {deviceInfo && (
            <span className="device-info">
              {isDeviceConfigured(deviceInfo.device)
                ? `${deviceInfo.device.name} v${deviceInfo.version} • ${deviceInfo.device.leds} LEDs`
                : `Safe Mode v${deviceInfo.version}`}
            </span>
          )}
        </div>
        <div className="connection-right">
          {batteryData && connectionState === 'connected' && (
            <div className="battery-status-header">
              {batteryData.n ? (
                <>
                  <div className="battery-indicator">
                    <div
                      className="battery-fill"
                      style={{
                        width: `${batteryData.p}%`,
                        backgroundColor:
                          batteryData.p > 50
                            ? '#22c55e'
                            : batteryData.p > 20
                              ? '#eab308'
                              : '#ef4444',
                      }}
                    />
                    {batteryData.c && <span className="battery-charging">⚡</span>}
                  </div>
                  <span className="battery-text">
                    {batteryData.p}% • {batteryData.v.toFixed(2)}V{batteryData.c ? ' • ⚡' : ''}
                  </span>
                  <button
                    className="battery-debug-btn"
                    onClick={() => setIsBatteryModalOpen(true)}
                    title="Show battery info"
                    aria-label="Show battery info"
                  >
                    ℹ️
                  </button>
                </>
              ) : (
                <>
                  <span className="battery-text">No Battery</span>
                  <button
                    className="battery-debug-btn"
                    onClick={() => setIsBatteryModalOpen(true)}
                    title="Show battery info"
                    aria-label="Show battery info"
                  >
                    ℹ️
                  </button>
                </>
              )}
            </div>
          )}
          <button
            className="btn btn-small console-btn"
            onClick={onOpenConsole}
            title="Open Serial Console"
            aria-label="Open Serial Console"
          >
            🖥️
          </button>
          <span className="status-indicator" style={{ backgroundColor: getStatusColor() }} />
          <span className="status-text">{getStatusText()}</span>
          {connectionState === 'connected' ? (
            <button className="btn btn-disconnect" onClick={onDisconnect}>
              Disconnect
            </button>
          ) : (
            <button
              className="btn btn-connect"
              onClick={onConnect}
              disabled={connectionState === 'connecting'}
            >
              {connectionState === 'connecting' ? 'Connecting...' : 'Connect'}
            </button>
          )}
        </div>
      </div>

      <BatteryModal
        isOpen={isBatteryModalOpen}
        onClose={() => setIsBatteryModalOpen(false)}
        onRefresh={onRequestBatteryStatus}
        statusData={batteryStatusData}
      />
    </>
  );
}
