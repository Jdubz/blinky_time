/**
 * Serial connection manager for blinky device
 */
import { SerialPort } from 'serialport';
import { ReadlineParser } from '@serialport/parser-readline';
import { EventEmitter } from 'events';
const BAUD_RATE = 115200;
const COMMAND_TIMEOUT_MS = 2000;
export class BlinkySerial extends EventEmitter {
    port = null;
    parser = null;
    portPath = null;
    deviceInfo = null;
    streaming = false;
    pendingCommand = null;
    // Multi-line response accumulation
    responseBuffer = [];
    responseTimeout = null;
    static RESPONSE_LINE_TIMEOUT_MS = 100; // Wait for more lines
    /**
     * List available serial ports
     */
    async listPorts() {
        const ports = await SerialPort.list();
        return ports.map(p => p.path);
    }
    /**
     * Connect to a serial port
     */
    async connect(portPath) {
        if (this.port) {
            await this.disconnect();
        }
        return new Promise((resolve, reject) => {
            this.port = new SerialPort({
                path: portPath,
                baudRate: BAUD_RATE,
            });
            this.parser = this.port.pipe(new ReadlineParser({ delimiter: '\n' }));
            this.portPath = portPath;
            this.port.on('error', (err) => {
                this.emit('error', err);
                reject(err);
            });
            this.port.on('close', () => {
                this.port = null;
                this.parser = null;
                this.portPath = null;
                this.deviceInfo = null;
                this.streaming = false;
                this.emit('disconnected');
            });
            this.parser.on('data', (line) => {
                this.handleLine(line.trim());
            });
            // Wait for port to open, then get device info
            this.port.on('open', async () => {
                try {
                    // Small delay for device to be ready
                    await new Promise(r => setTimeout(r, 500));
                    // Get device info
                    const infoJson = await this.sendCommand('json info');
                    const deviceInfo = JSON.parse(infoJson);
                    this.deviceInfo = deviceInfo;
                    this.emit('connected', deviceInfo);
                    resolve(deviceInfo);
                }
                catch (err) {
                    reject(err);
                }
            });
        });
    }
    /**
     * Disconnect from serial port
     */
    async disconnect() {
        if (this.streaming) {
            await this.stopStream();
        }
        // Clear any pending response state
        if (this.responseTimeout) {
            clearTimeout(this.responseTimeout);
            this.responseTimeout = null;
        }
        this.responseBuffer = [];
        if (this.port && this.port.isOpen) {
            return new Promise((resolve) => {
                this.port.close(() => {
                    this.port = null;
                    this.parser = null;
                    this.portPath = null;
                    this.deviceInfo = null;
                    resolve();
                });
            });
        }
    }
    /**
     * Get current connection state
     */
    getState() {
        return {
            connected: this.port !== null && this.port.isOpen,
            port: this.portPath,
            deviceInfo: this.deviceInfo,
            streaming: this.streaming,
        };
    }
    /**
     * Send a command and wait for response
     */
    async sendCommand(command) {
        if (!this.port || !this.port.isOpen) {
            throw new Error('Not connected');
        }
        // If streaming, temporarily stop to send command
        const wasStreaming = this.streaming;
        if (wasStreaming) {
            await this.stopStream();
        }
        return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
                this.pendingCommand = null;
                reject(new Error(`Command timeout: ${command}`));
            }, COMMAND_TIMEOUT_MS);
            this.pendingCommand = { resolve, reject, timeout };
            this.port.write(command + '\n');
        }).then(async (result) => {
            // Resume streaming if it was active
            if (wasStreaming) {
                await this.startStream();
            }
            return result;
        });
    }
    /**
     * Start audio streaming
     */
    async startStream() {
        if (!this.port || !this.port.isOpen) {
            throw new Error('Not connected');
        }
        this.port.write('stream on\n');
        this.streaming = true;
        this.emit('streamStarted');
    }
    /**
     * Stop audio streaming
     */
    async stopStream() {
        if (!this.port || !this.port.isOpen) {
            return;
        }
        this.port.write('stream off\n');
        this.streaming = false;
        this.emit('streamStopped');
    }
    /**
     * Get all settings as JSON
     */
    async getSettings() {
        const json = await this.sendCommand('json settings');
        const parsed = JSON.parse(json);
        return parsed.settings || [];
    }
    /**
     * Set a setting value
     */
    async setSetting(name, value) {
        return this.sendCommand(`set ${name} ${value}`);
    }
    /**
     * Save settings to flash
     */
    async saveSettings() {
        return this.sendCommand('save');
    }
    /**
     * Load settings from flash
     */
    async loadSettings() {
        return this.sendCommand('load');
    }
    /**
     * Reset to defaults
     */
    async resetDefaults() {
        return this.sendCommand('defaults');
    }
    /**
     * Handle incoming line from serial
     */
    handleLine(line) {
        // Check for JSON audio data (may include music mode: {"a":{...},"m":{...}})
        if (line.startsWith('{"a":')) {
            try {
                const parsed = JSON.parse(line);
                const audio = parsed.a;
                this.emit('audio', audio);
                // Emit transient events (unified detection using 't' field)
                // The simplified "Drummer's Algorithm" outputs a single transient strength
                if (audio.t > 0) {
                    this.emit('transient', { type: 'unified', strength: audio.t });
                }
                // Emit music mode state if present
                if (parsed.m) {
                    const music = parsed.m;
                    this.emit('music', music);
                    // Emit beat event (phase wrap detection)
                    if (music.q === 1) {
                        this.emit('beat', { type: 'quarter', bpm: music.bpm });
                    }
                }
                // Emit LED telemetry if present
                if (parsed.led) {
                    const led = parsed.led;
                    this.emit('led', led);
                }
            }
            catch {
                // Ignore parse errors
            }
            return;
        }
        // Check for JSON battery data
        if (line.startsWith('{"b":')) {
            try {
                const parsed = JSON.parse(line);
                const battery = parsed.b;
                this.emit('battery', battery);
            }
            catch {
                // Ignore parse errors
            }
            return;
        }
        // Check for pending command response
        if (this.pendingCommand) {
            // Clear any existing line timeout
            if (this.responseTimeout) {
                clearTimeout(this.responseTimeout);
                this.responseTimeout = null;
            }
            // Add line to buffer
            this.responseBuffer.push(line);
            // Set a short timeout to wait for more lines
            // If no more lines arrive within timeout, finalize the response
            this.responseTimeout = setTimeout(() => {
                if (this.pendingCommand) {
                    clearTimeout(this.pendingCommand.timeout);
                    const response = this.responseBuffer.join('\n');
                    this.responseBuffer = [];
                    this.responseTimeout = null;
                    this.pendingCommand.resolve(response);
                    this.pendingCommand = null;
                }
            }, BlinkySerial.RESPONSE_LINE_TIMEOUT_MS);
            return;
        }
        // Emit raw line for debugging
        this.emit('line', line);
    }
}
