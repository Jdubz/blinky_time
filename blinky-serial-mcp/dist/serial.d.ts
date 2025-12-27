/**
 * Serial connection manager for blinky device
 */
import { EventEmitter } from 'events';
import type { DeviceInfo, Setting, ConnectionState } from './types.js';
export declare class BlinkySerial extends EventEmitter {
    private port;
    private parser;
    private portPath;
    private deviceInfo;
    private streaming;
    private pendingCommand;
    /**
     * List available serial ports
     */
    listPorts(): Promise<string[]>;
    /**
     * Connect to a serial port
     */
    connect(portPath: string): Promise<DeviceInfo>;
    /**
     * Disconnect from serial port
     */
    disconnect(): Promise<void>;
    /**
     * Get current connection state
     */
    getState(): ConnectionState;
    /**
     * Send a command and wait for response
     */
    sendCommand(command: string): Promise<string>;
    /**
     * Start audio streaming
     */
    startStream(): Promise<void>;
    /**
     * Stop audio streaming
     */
    stopStream(): Promise<void>;
    /**
     * Get all settings as JSON
     */
    getSettings(): Promise<Setting[]>;
    /**
     * Set a setting value
     */
    setSetting(name: string, value: number): Promise<string>;
    /**
     * Save settings to flash
     */
    saveSettings(): Promise<string>;
    /**
     * Load settings from flash
     */
    loadSettings(): Promise<string>;
    /**
     * Reset to defaults
     */
    resetDefaults(): Promise<string>;
    /**
     * Handle incoming line from serial
     */
    private handleLine;
}
