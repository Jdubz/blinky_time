import { describe, it, expect, vi } from 'vitest';
import { render, screen, fireEvent } from '@testing-library/react';
import { BatteryModal } from './BatteryModal';
import type { BatteryStatusData } from '../services/serial';

describe('BatteryModal', () => {
  const mockBatteryData: BatteryStatusData = {
    voltage: 4.2,
    percent: 100,
    charging: true,
    connected: true,
  };

  const defaultProps = {
    isOpen: false,
    onClose: vi.fn(),
    statusData: null,
    onRefresh: vi.fn(),
  };

  it('does not render when isOpen is false', () => {
    const { container } = render(<BatteryModal {...defaultProps} />);
    expect(container).toBeEmptyDOMElement();
  });

  it('renders when isOpen is true', () => {
    render(<BatteryModal {...defaultProps} isOpen={true} statusData={mockBatteryData} />);
    expect(screen.getByText('100%')).toBeInTheDocument();
  });

  describe('battery status display', () => {
    it('shows battery percentage', () => {
      render(<BatteryModal {...defaultProps} isOpen={true} statusData={mockBatteryData} />);
      expect(screen.getByText('100%')).toBeInTheDocument();
    });

    it('shows battery voltage', () => {
      render(<BatteryModal {...defaultProps} isOpen={true} statusData={mockBatteryData} />);
      expect(screen.getByText('4.20V')).toBeInTheDocument();
    });

    it('shows charging status when charging', () => {
      render(<BatteryModal {...defaultProps} isOpen={true} statusData={mockBatteryData} />);
      expect(screen.getByText('⚡ Charging')).toBeInTheDocument();
    });

    it('shows on battery status when not charging', () => {
      const notChargingData = { ...mockBatteryData, charging: false };
      render(<BatteryModal {...defaultProps} isOpen={true} statusData={notChargingData} />);
      expect(screen.getByText('On Battery')).toBeInTheDocument();
    });
  });

  describe('battery level indicators', () => {
    it('shows excellent status for 90%+', () => {
      const excellentData = { ...mockBatteryData, percent: 95 };
      render(<BatteryModal {...defaultProps} isOpen={true} statusData={excellentData} />);
      expect(screen.getByText('Excellent')).toBeInTheDocument();
    });

    it('shows good status for 60-89%', () => {
      const goodData = { ...mockBatteryData, percent: 75 };
      render(<BatteryModal {...defaultProps} isOpen={true} statusData={goodData} />);
      expect(screen.getByText('Good')).toBeInTheDocument();
    });

    it('shows fair status for 30-59%', () => {
      const fairData = { ...mockBatteryData, percent: 45 };
      render(<BatteryModal {...defaultProps} isOpen={true} statusData={fairData} />);
      expect(screen.getByText('Fair')).toBeInTheDocument();
    });

    it('shows low status for 10-29%', () => {
      const lowData = { ...mockBatteryData, percent: 20 };
      render(<BatteryModal {...defaultProps} isOpen={true} statusData={lowData} />);
      expect(screen.getByText('Low')).toBeInTheDocument();
    });

    it('shows critical status for <10%', () => {
      const criticalData = { ...mockBatteryData, percent: 5 };
      render(<BatteryModal {...defaultProps} isOpen={true} statusData={criticalData} />);
      expect(screen.getByText('Critical')).toBeInTheDocument();
    });
  });

  describe('battery icons', () => {
    it('shows charging icon when charging', () => {
      const chargingData = { ...mockBatteryData, charging: true, percent: 50 };
      render(<BatteryModal {...defaultProps} isOpen={true} statusData={chargingData} />);
      expect(screen.getByText('🔌')).toBeInTheDocument();
    });

    it('shows battery icon when not charging and above 75%', () => {
      const fullData = { ...mockBatteryData, charging: false, percent: 80 };
      render(<BatteryModal {...defaultProps} isOpen={true} statusData={fullData} />);
      expect(screen.getByText('🔋')).toBeInTheDocument();
    });

    it('shows low battery icon when below 25%', () => {
      const lowData = { ...mockBatteryData, charging: false, percent: 10, voltage: 3.4 };
      render(<BatteryModal {...defaultProps} isOpen={true} statusData={lowData} />);
      expect(screen.getByText('🪫')).toBeInTheDocument();
    });
  });

  describe('low voltage warning', () => {
    it('shows warning when voltage is below 3.3V', () => {
      const lowVoltageData = { ...mockBatteryData, voltage: 3.25 };
      render(<BatteryModal {...defaultProps} isOpen={true} statusData={lowVoltageData} />);
      expect(screen.getByText(/Low battery! Charge soon to avoid shutdown/)).toBeInTheDocument();
    });

    it('does not show warning when voltage is above 3.3V', () => {
      const normalData = { ...mockBatteryData, voltage: 3.7 };
      render(<BatteryModal {...defaultProps} isOpen={true} statusData={normalData} />);
      expect(
        screen.queryByText(/Low battery! Charge soon to avoid shutdown/)
      ).not.toBeInTheDocument();
    });
  });

  describe('loading state', () => {
    it('shows loading message when statusData is null', () => {
      render(<BatteryModal {...defaultProps} isOpen={true} statusData={null} />);
      expect(screen.getByText('Loading battery data...')).toBeInTheDocument();
    });

    it('shows refresh button in loading state', () => {
      render(<BatteryModal {...defaultProps} isOpen={true} statusData={null} />);
      const refreshButtons = screen.getAllByRole('button', { name: 'Refresh' });
      expect(refreshButtons.length).toBeGreaterThanOrEqual(1);
    });
  });

  describe('button interactions', () => {
    it('calls onClose when Close button is clicked', () => {
      const onClose = vi.fn();
      render(
        <BatteryModal
          {...defaultProps}
          isOpen={true}
          statusData={mockBatteryData}
          onClose={onClose}
        />
      );

      fireEvent.click(screen.getByRole('button', { name: 'Close' }));
      expect(onClose).toHaveBeenCalledTimes(1);
    });

    it('calls onRefresh when Refresh button is clicked', () => {
      const onRefresh = vi.fn();
      render(
        <BatteryModal
          {...defaultProps}
          isOpen={true}
          statusData={mockBatteryData}
          onRefresh={onRefresh}
        />
      );

      const refreshButtons = screen.getAllByRole('button', { name: 'Refresh' });
      fireEvent.click(refreshButtons[0]);
      expect(onRefresh).toHaveBeenCalledTimes(1);
    });

    it('calls onClose when clicking overlay background', () => {
      const onClose = vi.fn();
      render(
        <BatteryModal
          {...defaultProps}
          isOpen={true}
          statusData={mockBatteryData}
          onClose={onClose}
        />
      );

      const overlay = screen.getByRole('button', { name: 'Close' }).closest('.modal-overlay');
      if (overlay) {
        fireEvent.click(overlay);
        expect(onClose).toHaveBeenCalledTimes(1);
      }
    });
  });

  describe('CSS class application', () => {
    it('applies high battery level class for >75%', () => {
      render(
        <BatteryModal
          {...defaultProps}
          isOpen={true}
          statusData={{ ...mockBatteryData, percent: 80 }}
        />
      );
      const percentElement = screen.getByText('80%');
      expect(percentElement).toHaveClass('battery-level-high');
    });

    it('applies medium battery level class for 50-74%', () => {
      render(
        <BatteryModal
          {...defaultProps}
          isOpen={true}
          statusData={{ ...mockBatteryData, percent: 60 }}
        />
      );
      const percentElement = screen.getByText('60%');
      expect(percentElement).toHaveClass('battery-level-medium');
    });

    it('applies low battery level class for 25-49%', () => {
      render(
        <BatteryModal
          {...defaultProps}
          isOpen={true}
          statusData={{ ...mockBatteryData, percent: 30 }}
        />
      );
      const percentElement = screen.getByText('30%');
      expect(percentElement).toHaveClass('battery-level-low');
    });

    it('applies critical battery level class for <25%', () => {
      render(
        <BatteryModal
          {...defaultProps}
          isOpen={true}
          statusData={{ ...mockBatteryData, percent: 10 }}
        />
      );
      const percentElement = screen.getByText('10%');
      expect(percentElement).toHaveClass('battery-level-critical');
    });
  });
});
