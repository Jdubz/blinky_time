import { useState, useRef, useEffect, KeyboardEvent } from 'react';
import { ConsoleEntry } from '../types';

interface ConsoleProps {
  entries: ConsoleEntry[];
  onSendCommand: (command: string) => void;
  onClear: () => void;
  disabled: boolean;
}

export function Console({ entries, onSendCommand, onClear, disabled }: ConsoleProps) {
  const [input, setInput] = useState('');
  const [history, setHistory] = useState<string[]>([]);
  const [historyIndex, setHistoryIndex] = useState(-1);
  const logEndRef = useRef<HTMLDivElement>(null);
  const inputRef = useRef<HTMLInputElement>(null);

  // Auto-scroll to bottom when new entries arrive
  useEffect(() => {
    if (logEndRef.current) {
      logEndRef.current.scrollIntoView({ behavior: 'smooth' });
    }
  }, [entries]);

  const handleSubmit = () => {
    const command = input.trim();
    if (!command || disabled) return;

    onSendCommand(command);
    setHistory(prev => [...prev.slice(-50), command]); // Keep last 50 commands
    setHistoryIndex(-1);
    setInput('');
  };

  const handleKeyDown = (e: KeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'Enter') {
      handleSubmit();
    } else if (e.key === 'ArrowUp') {
      e.preventDefault();
      if (history.length > 0) {
        const newIndex = historyIndex < history.length - 1 ? historyIndex + 1 : historyIndex;
        setHistoryIndex(newIndex);
        setInput(history[history.length - 1 - newIndex] || '');
      }
    } else if (e.key === 'ArrowDown') {
      e.preventDefault();
      if (historyIndex > 0) {
        const newIndex = historyIndex - 1;
        setHistoryIndex(newIndex);
        setInput(history[history.length - 1 - newIndex] || '');
      } else if (historyIndex === 0) {
        setHistoryIndex(-1);
        setInput('');
      }
    }
  };

  const formatTime = (date: Date) => {
    return date.toLocaleTimeString('en-US', {
      hour12: false,
      hour: '2-digit',
      minute: '2-digit',
      second: '2-digit'
    });
  };

  const getEntryClass = (type: ConsoleEntry['type']) => {
    switch (type) {
      case 'sent': return 'console-sent';
      case 'received': return 'console-received';
      case 'error': return 'console-error';
      case 'info': return 'console-info';
      default: return '';
    }
  };

  const getEntryPrefix = (type: ConsoleEntry['type']) => {
    switch (type) {
      case 'sent': return '>';
      case 'received': return '<';
      case 'error': return '!';
      case 'info': return '*';
      default: return ' ';
    }
  };

  return (
    <div className="console">
      <div className="console-header">
        <h2>Console</h2>
        <button className="btn btn-small" onClick={onClear}>
          Clear
        </button>
      </div>
      <div className="console-log">
        {entries.length === 0 && (
          <div className="console-empty">
            {disabled
              ? 'Connect to device to see serial output'
              : 'Serial output will appear here...'}
          </div>
        )}
        {entries.map(entry => (
          <div key={entry.id} className={`console-entry ${getEntryClass(entry.type)}`}>
            <span className="console-time">{formatTime(entry.timestamp)}</span>
            <span className="console-prefix">{getEntryPrefix(entry.type)}</span>
            <span className="console-message">{entry.message}</span>
          </div>
        ))}
        <div ref={logEndRef} />
      </div>
      <div className="console-input-row">
        <input
          ref={inputRef}
          type="text"
          className="console-input"
          value={input}
          onChange={(e) => setInput(e.target.value)}
          onKeyDown={handleKeyDown}
          placeholder={disabled ? 'Connect to send commands...' : 'Type command and press Enter...'}
          disabled={disabled}
        />
        <button
          className="btn btn-primary"
          onClick={handleSubmit}
          disabled={disabled || !input.trim()}
        >
          Send
        </button>
      </div>
    </div>
  );
}
