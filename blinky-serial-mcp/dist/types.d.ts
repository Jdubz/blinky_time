/**
 * Types for blinky device serial communication
 */
export interface DeviceInfo {
    device: string;
    version: string;
    width: number;
    height: number;
    leds: number;
}
export interface AudioSample {
    l: number;
    t: number;
    pk: number;
    vl: number;
    raw: number;
    h: number;
    alive: number;
    z: number;
    avg?: number;
    prev?: number;
}
export interface MusicModeState {
    a: number;
    bpm: number;
    ph: number;
    conf: number;
    q: number;
    h: number;
    w: number;
    sb?: number;
    mb?: number;
    pe?: number;
    ei?: number;
}
export interface LedTelemetry {
    tot: number;
    pct: number;
}
export interface BeatEvent {
    timestampMs: number;
    bpm: number;
    type: 'quarter' | 'half' | 'whole';
}
export interface MusicModeMetrics {
    bpmAccuracy: number;
    expectedBPM: number;
    detectedBPM: number;
    activationTimeMs: number;
    beatF1Score: number;
    confidenceAvg: number;
}
export interface BatteryStatus {
    n: boolean;
    c: boolean;
    v: number;
    p: number;
}
export interface Setting {
    name: string;
    value: number;
    type: 'uint8' | 'uint16' | 'int8' | 'float';
    cat: string;
    min: number;
    max: number;
    desc?: string;
}
export interface ConnectionState {
    connected: boolean;
    port: string | null;
    deviceInfo: DeviceInfo | null;
    streaming: boolean;
}
export interface TestResult {
    pattern: string;
    duration: number;
    detections: TransientEvent[];
    metrics: TestMetrics;
}
export interface TransientEvent {
    timestampMs: number;
    type: 'low' | 'high' | 'unified';
    strength: number;
}
export interface TestMetrics {
    f1Score: number;
    precision: number;
    recall: number;
    truePositives: number;
    falsePositives: number;
    falseNegatives: number;
    expectedTotal: number;
    avgTimingErrorMs: number | null;
    audioLatencyMs: number;
}
