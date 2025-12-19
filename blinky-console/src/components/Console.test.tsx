import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render, screen, fireEvent } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { Console } from './Console';
import { ConsoleEntry } from '../types';

describe('Console', () => {
  const mockEntries: ConsoleEntry[] = [
    { id: 1, timestamp: new Date('2024-01-01T10:00:00'), type: 'sent', message: 'json info' },
    {
      id: 2,
      timestamp: new Date('2024-01-01T10:00:01'),
      type: 'received',
      message: '{"device":"test"}',
    },
    {
      id: 3,
      timestamp: new Date('2024-01-01T10:00:02'),
      type: 'error',
      message: 'Connection lost',
    },
    { id: 4, timestamp: new Date('2024-01-01T10:00:03'), type: 'info', message: 'Reconnecting...' },
  ];

  const defaultProps = {
    entries: [],
    onSendCommand: vi.fn(),
    onClear: vi.fn(),
    disabled: false,
  };

  beforeEach(() => {
    vi.clearAllMocks();
  });

  it('renders the console header', () => {
    render(<Console {...defaultProps} />);
    expect(screen.getByText('Console')).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Clear' })).toBeInTheDocument();
  });

  describe('empty state', () => {
    it('shows placeholder when disabled and no entries', () => {
      render(<Console {...defaultProps} disabled={true} />);
      expect(screen.getByText('Connect to device to see serial output')).toBeInTheDocument();
    });

    it('shows placeholder when enabled and no entries', () => {
      render(<Console {...defaultProps} />);
      expect(screen.getByText('Serial output will appear here...')).toBeInTheDocument();
    });
  });

  describe('entry display', () => {
    it('renders all console entries', () => {
      render(<Console {...defaultProps} entries={mockEntries} />);
      expect(screen.getByText('json info')).toBeInTheDocument();
      expect(screen.getByText('{"device":"test"}')).toBeInTheDocument();
      expect(screen.getByText('Connection lost')).toBeInTheDocument();
      expect(screen.getByText('Reconnecting...')).toBeInTheDocument();
    });

    it('displays correct prefix for each entry type', () => {
      render(<Console {...defaultProps} entries={mockEntries} />);
      const prefixes = screen.getAllByText(/^[><!*]$/);
      expect(prefixes).toHaveLength(4);
    });

    it('formats timestamps correctly', () => {
      render(<Console {...defaultProps} entries={mockEntries} />);
      expect(screen.getByText('10:00:00')).toBeInTheDocument();
      expect(screen.getByText('10:00:01')).toBeInTheDocument();
    });

    it('truncates long messages', () => {
      const longMessage = 'x'.repeat(600);
      const entries: ConsoleEntry[] = [
        { id: 1, timestamp: new Date(), type: 'received', message: longMessage },
      ];
      render(<Console {...defaultProps} entries={entries} />);

      const messageElement = screen.getByText(/x+\.\.\./);
      expect(messageElement.textContent?.length).toBeLessThanOrEqual(503); // 500 chars + '...'
    });
  });

  describe('command input', () => {
    it('has disabled input when disabled prop is true', () => {
      render(<Console {...defaultProps} disabled={true} />);
      const input = screen.getByPlaceholderText('Connect to send commands...');
      expect(input).toBeDisabled();
    });

    it('has enabled input when disabled prop is false', () => {
      render(<Console {...defaultProps} />);
      const input = screen.getByPlaceholderText('Type command and press Enter...');
      expect(input).not.toBeDisabled();
    });

    it('calls onSendCommand when Enter is pressed', async () => {
      const onSendCommand = vi.fn();
      render(<Console {...defaultProps} onSendCommand={onSendCommand} />);

      const input = screen.getByPlaceholderText('Type command and press Enter...');
      await userEvent.type(input, 'test command{enter}');

      expect(onSendCommand).toHaveBeenCalledWith('test command');
    });

    it('calls onSendCommand when Send button is clicked', async () => {
      const onSendCommand = vi.fn();
      render(<Console {...defaultProps} onSendCommand={onSendCommand} />);

      const input = screen.getByPlaceholderText('Type command and press Enter...');
      await userEvent.type(input, 'test command');

      fireEvent.click(screen.getByRole('button', { name: 'Send' }));
      expect(onSendCommand).toHaveBeenCalledWith('test command');
    });

    it('clears input after sending command', async () => {
      render(<Console {...defaultProps} />);

      const input = screen.getByPlaceholderText(
        'Type command and press Enter...'
      ) as HTMLInputElement;
      await userEvent.type(input, 'test command{enter}');

      expect(input.value).toBe('');
    });

    it('does not send empty commands', async () => {
      const onSendCommand = vi.fn();
      render(<Console {...defaultProps} onSendCommand={onSendCommand} />);

      const input = screen.getByPlaceholderText('Type command and press Enter...');
      fireEvent.keyDown(input, { key: 'Enter' });

      expect(onSendCommand).not.toHaveBeenCalled();
    });

    it('trims whitespace from commands', async () => {
      const onSendCommand = vi.fn();
      render(<Console {...defaultProps} onSendCommand={onSendCommand} />);

      const input = screen.getByPlaceholderText('Type command and press Enter...');
      await userEvent.type(input, '  test command  {enter}');

      expect(onSendCommand).toHaveBeenCalledWith('test command');
    });
  });

  describe('command history', () => {
    it('navigates history with up arrow', async () => {
      const onSendCommand = vi.fn();
      render(<Console {...defaultProps} onSendCommand={onSendCommand} />);

      const input = screen.getByPlaceholderText(
        'Type command and press Enter...'
      ) as HTMLInputElement;

      // Send some commands
      await userEvent.type(input, 'first{enter}');
      await userEvent.type(input, 'second{enter}');

      // Navigate history
      fireEvent.keyDown(input, { key: 'ArrowUp' });
      expect(input.value).toBe('second');

      fireEvent.keyDown(input, { key: 'ArrowUp' });
      expect(input.value).toBe('first');
    });

    it('navigates history with down arrow', async () => {
      const onSendCommand = vi.fn();
      render(<Console {...defaultProps} onSendCommand={onSendCommand} />);

      const input = screen.getByPlaceholderText(
        'Type command and press Enter...'
      ) as HTMLInputElement;

      // Send some commands
      await userEvent.type(input, 'first{enter}');
      await userEvent.type(input, 'second{enter}');

      // Navigate up then down
      fireEvent.keyDown(input, { key: 'ArrowUp' });
      fireEvent.keyDown(input, { key: 'ArrowUp' });
      fireEvent.keyDown(input, { key: 'ArrowDown' });
      expect(input.value).toBe('second');

      fireEvent.keyDown(input, { key: 'ArrowDown' });
      expect(input.value).toBe('');
    });
  });

  describe('clear functionality', () => {
    it('calls onClear when Clear button is clicked', () => {
      const onClear = vi.fn();
      render(<Console {...defaultProps} onClear={onClear} />);

      fireEvent.click(screen.getByRole('button', { name: 'Clear' }));
      expect(onClear).toHaveBeenCalledTimes(1);
    });
  });

  describe('Send button state', () => {
    it('is disabled when input is empty', () => {
      render(<Console {...defaultProps} />);
      const sendButton = screen.getByRole('button', { name: 'Send' });
      expect(sendButton).toBeDisabled();
    });

    it('is disabled when console is disabled', () => {
      render(<Console {...defaultProps} disabled={true} />);
      const sendButton = screen.getByRole('button', { name: 'Send' });
      expect(sendButton).toBeDisabled();
    });

    it('is enabled when input has text and console is enabled', async () => {
      render(<Console {...defaultProps} />);
      const input = screen.getByPlaceholderText('Type command and press Enter...');
      await userEvent.type(input, 'test');

      const sendButton = screen.getByRole('button', { name: 'Send' });
      expect(sendButton).not.toBeDisabled();
    });
  });
});
