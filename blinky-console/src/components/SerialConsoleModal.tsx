import { useEffect, useState, useRef } from 'react';
import './SerialConsoleModal.css';

interface SerialConsoleModalProps {
  isOpen: boolean;
  onClose: () => void;
  onSendCommand: (command: string) => void;
  consoleLines: string[];
  disabled: boolean;
}

export function SerialConsoleModal({
  isOpen,
  onClose,
  onSendCommand,
  consoleLines,
  disabled,
}: SerialConsoleModalProps) {
  const [command, setCommand] = useState('');
  const [commandHistory, setCommandHistory] = useState<string[]>([]);
  const [historyIndex, setHistoryIndex] = useState(-1);
  const consoleEndRef = useRef<HTMLDivElement>(null);
  const inputRef = useRef<HTMLInputElement>(null);

  // Auto-scroll to bottom when new lines appear
  useEffect(() => {
    if (consoleEndRef.current) {
      consoleEndRef.current.scrollIntoView({ behavior: 'smooth' });
    }
  }, [consoleLines]);

  // Focus input when modal opens
  useEffect(() => {
    if (isOpen && inputRef.current) {
      inputRef.current.focus();
    }
  }, [isOpen]);

  const handleSendCommand = () => {
    if (!command.trim() || disabled) return;

    onSendCommand(command);
    setCommandHistory(prev => [...prev, command]);
    setCommand('');
    setHistoryIndex(-1);
  };

  const handleKeyDown = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'Enter') {
      handleSendCommand();
    } else if (e.key === 'ArrowUp') {
      e.preventDefault();
      if (commandHistory.length > 0) {
        const newIndex =
          historyIndex === -1 ? commandHistory.length - 1 : Math.max(0, historyIndex - 1);
        setHistoryIndex(newIndex);
        setCommand(commandHistory[newIndex]);
      }
    } else if (e.key === 'ArrowDown') {
      e.preventDefault();
      if (historyIndex !== -1) {
        const newIndex = historyIndex + 1;
        if (newIndex >= commandHistory.length) {
          setHistoryIndex(-1);
          setCommand('');
        } else {
          setHistoryIndex(newIndex);
          setCommand(commandHistory[newIndex]);
        }
      }
    }
  };

  if (!isOpen) return null;

  return (
    <div className="modal-overlay" onClick={onClose}>
      <div className="console-modal-content" onClick={e => e.stopPropagation()}>
        <div className="modal-header">
          <h2>Serial Console</h2>
          <button className="modal-close" onClick={onClose}>
            ×
          </button>
        </div>

        <div className="console-body">
          <div className="console-output">
            {consoleLines.length === 0 ? (
              <div className="console-empty">
                <p>No serial data yet. Send a command to get started.</p>
                <p className="console-hint">Try: info, help, battery debug</p>
              </div>
            ) : (
              consoleLines.map((line, index) => (
                <div key={index} className="console-line">
                  {line}
                </div>
              ))
            )}
            <div ref={consoleEndRef} />
          </div>

          <div className="console-input-area">
            <input
              ref={inputRef}
              type="text"
              className="console-input"
              value={command}
              onChange={e => setCommand(e.target.value)}
              onKeyDown={handleKeyDown}
              placeholder={disabled ? 'Connect to device first' : 'Enter command...'}
              disabled={disabled}
            />
            <button
              className="btn btn-primary"
              onClick={handleSendCommand}
              disabled={disabled || !command.trim()}
            >
              Send
            </button>
          </div>

          <div className="console-hints">
            <span>Press Enter to send • ↑/↓ for command history</span>
          </div>
        </div>

        <div className="modal-footer">
          <button className="btn" onClick={onClose}>
            Close
          </button>
        </div>
      </div>
    </div>
  );
}
