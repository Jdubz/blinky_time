import { describe, it, expect, vi } from 'vitest';
import { render, screen, fireEvent } from '@testing-library/react';
import { ConnectionBar } from './ConnectionBar';
import { DeviceInfo, ConnectionState } from '../types';

describe('ConnectionBar', () => {
  const mockDeviceInfo: DeviceInfo = {
    device: 'Blinky Time',
    version: '1.0.0',
    width: 16,
    height: 16,
    leds: 256,
  };

  const defaultProps = {
    connectionState: 'disconnected' as ConnectionState,
    deviceInfo: null,
    batteryData: null,
    batteryStatusData: null,
    isSupported: true,
    errorMessage: null,
    onConnect: vi.fn(),
    onDisconnect: vi.fn(),
    onOpenConsole: vi.fn(),
    onRequestBatteryStatus: vi.fn(),
  };

  it('renders the app title', () => {
    render(<ConnectionBar {...defaultProps} />);
    expect(screen.getByText('Blinky Console')).toBeInTheDocument();
  });

  it('shows WebSerial not supported message when isSupported is false', () => {
    render(<ConnectionBar {...defaultProps} isSupported={false} />);
    expect(screen.getByText(/WebSerial not supported/)).toBeInTheDocument();
    expect(screen.queryByRole('button')).not.toBeInTheDocument();
  });

  describe('connection states', () => {
    it('shows "Disconnected" status when disconnected', () => {
      render(<ConnectionBar {...defaultProps} connectionState="disconnected" />);
      expect(screen.getByText('Disconnected')).toBeInTheDocument();
      expect(screen.getByRole('button', { name: 'Connect' })).toBeInTheDocument();
    });

    it('shows "Connecting..." status when connecting', () => {
      render(<ConnectionBar {...defaultProps} connectionState="connecting" />);
      expect(screen.getByText('Connecting...', { selector: '.status-text' })).toBeInTheDocument();
      const button = screen.getByRole('button', { name: 'Connecting...' });
      expect(button).toBeDisabled();
      expect(button).toHaveTextContent('Connecting...');
    });

    it('shows "Connected" status when connected', () => {
      render(<ConnectionBar {...defaultProps} connectionState="connected" />);
      expect(screen.getByText('Connected')).toBeInTheDocument();
      expect(screen.getByRole('button', { name: 'Disconnect' })).toBeInTheDocument();
    });

    it('shows "Connection Error" status when error occurs without message', () => {
      render(<ConnectionBar {...defaultProps} connectionState="error" errorMessage={null} />);
      expect(screen.getByText('Connection Error')).toBeInTheDocument();
    });

    it('shows error message when error occurs with message', () => {
      render(
        <ConnectionBar {...defaultProps} connectionState="error" errorMessage="Device not found" />
      );
      expect(screen.getByText('Error: Device not found')).toBeInTheDocument();
    });
  });

  describe('device info display', () => {
    it('shows device info when connected', () => {
      render(
        <ConnectionBar {...defaultProps} connectionState="connected" deviceInfo={mockDeviceInfo} />
      );
      expect(screen.getByText(/Blinky Time v1.0.0/)).toBeInTheDocument();
      expect(screen.getByText(/256 LEDs/)).toBeInTheDocument();
    });

    it('does not show device info when disconnected', () => {
      render(<ConnectionBar {...defaultProps} deviceInfo={null} />);
      expect(screen.queryByText(/LEDs/)).not.toBeInTheDocument();
    });
  });

  describe('button interactions', () => {
    it('calls onConnect when Connect button is clicked', () => {
      const onConnect = vi.fn();
      render(<ConnectionBar {...defaultProps} onConnect={onConnect} />);

      fireEvent.click(screen.getByRole('button', { name: 'Connect' }));
      expect(onConnect).toHaveBeenCalledTimes(1);
    });

    it('calls onDisconnect when Disconnect button is clicked', () => {
      const onDisconnect = vi.fn();
      render(
        <ConnectionBar {...defaultProps} connectionState="connected" onDisconnect={onDisconnect} />
      );

      fireEvent.click(screen.getByRole('button', { name: 'Disconnect' }));
      expect(onDisconnect).toHaveBeenCalledTimes(1);
    });
  });

  describe('status indicator colors', () => {
    it('uses green for connected state', () => {
      render(<ConnectionBar {...defaultProps} connectionState="connected" />);
      const indicator = document.querySelector('.status-indicator');
      expect(indicator).toHaveStyle({ backgroundColor: '#4ade80' });
    });

    it('uses yellow for connecting state', () => {
      render(<ConnectionBar {...defaultProps} connectionState="connecting" />);
      const indicator = document.querySelector('.status-indicator');
      expect(indicator).toHaveStyle({ backgroundColor: '#facc15' });
    });

    it('uses red for error state', () => {
      render(<ConnectionBar {...defaultProps} connectionState="error" />);
      const indicator = document.querySelector('.status-indicator');
      expect(indicator).toHaveStyle({ backgroundColor: '#f87171' });
    });

    it('uses gray for disconnected state', () => {
      render(<ConnectionBar {...defaultProps} connectionState="disconnected" />);
      const indicator = document.querySelector('.status-indicator');
      expect(indicator).toHaveStyle({ backgroundColor: '#6b7280' });
    });
  });
});
