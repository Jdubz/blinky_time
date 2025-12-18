#!/usr/bin/env python3
"""
Audio Tuner for Blinky Fire Effect

Records audio metrics, visualizes the signal chain, and allows
interactive parameter tuning to optimize kick/snare detection.

Usage:
  python audio_tuner.py record    - Record 30 seconds of data
  python audio_tuner.py analyze   - Analyze and tune parameters
  python audio_tuner.py apply     - Apply tuned parameters to device
"""

import serial
import time
import sys
import json
from datetime import datetime

PORT = 'COM34'
BAUD = 115200
DATA_FILE = 'audio_recording.json'

class AudioRecorder:
    def __init__(self, port=PORT, baud=BAUD):
        self.port = port
        self.baud = baud
        self.data = []

    def record(self, duration=30):
        """Record audio metrics for specified duration."""
        print(f"Connecting to {self.port}...")

        with serial.Serial(self.port, self.baud, timeout=1) as ser:
            time.sleep(0.5)

            # Enable detailed debug mode
            ser.write(b'mic fast\r\n')
            time.sleep(0.3)
            ser.read(ser.in_waiting)  # Clear buffer

            print(f"Recording for {duration} seconds...")
            print("Play music with clear kicks and snares!\n")

            start_time = time.time()
            samples = []

            while time.time() - start_time < duration:
                if ser.in_waiting:
                    line = ser.readline().decode(errors='ignore').strip()
                    if 'lvl=' in line:
                        sample = self._parse_line(line, time.time() - start_time)
                        if sample:
                            samples.append(sample)
                            self._print_progress(sample)
                time.sleep(0.005)

            ser.write(b'mic off\r\n')

        self.data = samples
        print(f"\n\nRecorded {len(samples)} samples")
        return samples

    def _parse_line(self, line, timestamp):
        """Parse a debug line into metrics dict."""
        try:
            sample = {'t': round(timestamp, 3)}
            parts = line.split()
            for p in parts:
                if '=' in p:
                    key, val = p.split('=')
                    if key == 'lvl':
                        sample['energy'] = float(val)
                    elif key == 'avg':
                        sample['slowAvg'] = float(val)
                    elif key == 'thr':
                        sample['threshold'] = float(val)
                    elif key == 'hit':
                        sample['hit'] = float(val)
                    elif key == 'spk':
                        sample['sparks'] = int(val)
                    elif key == 'act':
                        sample['activity'] = int(val.rstrip('%'))
                    elif key == 'gain':
                        sample['gain'] = float(val)
            return sample if 'energy' in sample else None
        except Exception:
            return None

    def _print_progress(self, sample):
        """Print live progress indicator."""
        bar_len = 30
        energy_bar = int(sample.get('energy', 0) * bar_len)
        energy_str = '#' * energy_bar + '-' * (bar_len - energy_bar)

        thresh = sample.get('threshold', 0)
        above = sample.get('energy', 0) > thresh
        marker = '*' if above else ' '
        print(f"\rE:[{energy_str}] {sample.get('energy', 0):.2f}{marker} "
              f"thr={thresh:.2f}  "
              f"H:{sample.get('hit', 0):.2f}  "
              f"S:{sample.get('sparks', 1):2d}  "
              f"A:{sample.get('activity', 0):2d}%", end='')

    def save(self, filename=DATA_FILE):
        """Save recorded data to JSON file."""
        with open(filename, 'w') as f:
            json.dump({
                'recorded_at': datetime.now().isoformat(),
                'samples': self.data
            }, f, indent=2)
        print(f"Saved {len(self.data)} samples to {filename}")

    def load(self, filename=DATA_FILE):
        """Load recorded data from JSON file."""
        with open(filename, 'r') as f:
            data = json.load(f)
        self.data = data['samples']
        print(f"Loaded {len(self.data)} samples from {filename}")
        print(f"Recorded at: {data.get('recorded_at', 'unknown')}")
        return self.data


class AudioAnalyzer:
    def __init__(self, data):
        self.data = data
        self.params = {
            'transientFactor': 2.0,
            'loudFloor': 0.08,
            'energyThreshold': 0.4,
            'cooldownMs': 120
        }

    def analyze(self):
        """Analyze recorded data and print statistics."""
        if not self.data:
            print("No data to analyze!")
            return

        energies = [s['energy'] for s in self.data]
        hits = [s['hit'] for s in self.data]
        activities = [s['activity'] for s in self.data]

        print("\n" + "="*60)
        print("RECORDING ANALYSIS")
        print("="*60)

        print(f"\nDuration: {self.data[-1]['t']:.1f} seconds")
        print(f"Samples: {len(self.data)} ({len(self.data)/self.data[-1]['t']:.1f} Hz)")

        print(f"\n--- Energy (mic level) ---")
        print(f"  Min: {min(energies):.3f}")
        print(f"  Max: {max(energies):.3f}")
        print(f"  Avg: {sum(energies)/len(energies):.3f}")
        print(f"  >0.3: {sum(1 for e in energies if e > 0.3)} samples ({100*sum(1 for e in energies if e > 0.3)/len(energies):.1f}%)")
        print(f"  >0.4: {sum(1 for e in energies if e > 0.4)} samples ({100*sum(1 for e in energies if e > 0.4)/len(energies):.1f}%)")
        print(f"  >0.5: {sum(1 for e in energies if e > 0.5)} samples ({100*sum(1 for e in energies if e > 0.5)/len(energies):.1f}%)")

        # Count actual hit triggers (hit > 0.9 after being < 0.5)
        new_hits = []
        for i, h in enumerate(hits):
            if h > 0.9 and (i == 0 or hits[i-1] < 0.5):
                new_hits.append(i)

        print(f"\n--- Transient Detection ---")
        print(f"  Hit triggers: {len(new_hits)}")
        print(f"  Hits per second: {len(new_hits)/self.data[-1]['t']:.2f}")

        # Threshold analysis if available
        thresholds = [s.get('threshold', 0) for s in self.data]
        slowAvgs = [s.get('slowAvg', 0) for s in self.data]
        if any(t > 0 for t in thresholds):
            print(f"\n--- Threshold Analysis ---")
            print(f"  slowAvg range: {min(slowAvgs):.3f} - {max(slowAvgs):.3f}")
            print(f"  threshold range: {min(thresholds):.3f} - {max(thresholds):.3f}")
            # How often is energy above threshold?
            above_thresh = sum(1 for i, e in enumerate(energies) if e > thresholds[i])
            print(f"  Energy > threshold: {above_thresh} samples ({100*above_thresh/len(energies):.1f}%)")
            # Margin at hit triggers
            if new_hits:
                margins = [(energies[i] - thresholds[i]) for i in new_hits]
                print(f"  Margin at hits: {min(margins):.3f} - {max(margins):.3f} (avg {sum(margins)/len(margins):.3f})")

        print(f"\n--- LED Activity ---")
        print(f"  Max: {max(activities)}%")
        print(f"  Avg: {sum(activities)/len(activities):.1f}%")
        print(f"  >15%: {sum(1 for a in activities if a > 15)} samples")

        # Spark statistics if available
        sparks = [s.get('sparks', 1) for s in self.data]
        if any(s > 1 for s in sparks):
            print(f"\n--- Spark Count ---")
            print(f"  Max: {max(sparks)}")
            print(f"  Avg: {sum(sparks)/len(sparks):.1f}")
            print(f"  >3: {sum(1 for s in sparks if s > 3)} samples")

        # Find energy levels at hit triggers
        if new_hits:
            hit_energies = [energies[i] for i in new_hits]
            print(f"\n--- Energy at Hit Triggers ---")
            print(f"  Min: {min(hit_energies):.3f}")
            print(f"  Max: {max(hit_energies):.3f}")
            print(f"  Avg: {sum(hit_energies)/len(hit_energies):.3f}")

        return {
            'duration': self.data[-1]['t'],
            'avg_energy': sum(energies)/len(energies),
            'max_energy': max(energies),
            'hit_count': len(new_hits),
            'hits_per_sec': len(new_hits)/self.data[-1]['t'],
            'avg_activity': sum(activities)/len(activities),
            'max_activity': max(activities)
        }

    def simulate(self, params=None):
        """Simulate transient detection with given parameters."""
        if params:
            self.params.update(params)

        p = self.params
        print(f"\n--- Simulating with: tfactor={p['transientFactor']}, "
              f"floor={p['loudFloor']}, energyThresh={p['energyThreshold']} ---")

        # Simulate slowAvg and transient detection
        slowAvg = 0.1
        slowAlpha = 0.025
        lastHitIdx = -1000
        cooldownSamples = int(p['cooldownMs'] / 33)  # ~30Hz sample rate

        simulated_hits = []
        simulated_sparks = []

        for i, sample in enumerate(self.data):
            energy = sample['energy']

            # Update slowAvg
            slowAvg += slowAlpha * (energy - slowAvg)

            # Check for transient
            threshold = max(p['loudFloor'], slowAvg * p['transientFactor'])
            cooldownOk = (i - lastHitIdx) > cooldownSamples

            hit = 0
            if cooldownOk and energy > threshold:
                hit = 1.0
                lastHitIdx = i
                simulated_hits.append(i)

            # Simulate spark count
            sparks = 1
            if energy > p['energyThreshold']:
                sparks = 2 + int((energy - p['energyThreshold']) * 10)
            if hit > 0.1:
                sparks += 4 + int(hit * 8)

            simulated_sparks.append(sparks)

        hits_per_sec = len(simulated_hits) / self.data[-1]['t']
        avg_sparks = sum(simulated_sparks) / len(simulated_sparks)

        print(f"  Simulated hits: {len(simulated_hits)} ({hits_per_sec:.2f}/sec)")
        print(f"  Avg sparks/frame: {avg_sparks:.2f}")

        # Show hit timing
        if simulated_hits and len(simulated_hits) < 50:
            intervals = []
            for i in range(1, len(simulated_hits)):
                dt = self.data[simulated_hits[i]]['t'] - self.data[simulated_hits[i-1]]['t']
                intervals.append(dt)
            if intervals:
                print(f"  Avg interval between hits: {sum(intervals)/len(intervals)*1000:.0f}ms")

        return {
            'hits': len(simulated_hits),
            'hits_per_sec': hits_per_sec,
            'avg_sparks': avg_sparks
        }

    def find_optimal(self):
        """Search for optimal parameters."""
        print("\n" + "="*60)
        print("SEARCHING FOR OPTIMAL PARAMETERS")
        print("="*60)

        # Target: 1-4 hits per second for typical music
        target_hps = 2.0
        best_params = None
        best_score = float('inf')

        results = []

        for tf in [1.5, 1.8, 2.0, 2.2, 2.5, 3.0]:
            for floor in [0.05, 0.08, 0.12, 0.15]:
                for ethresh in [0.3, 0.4, 0.5]:
                    params = {
                        'transientFactor': tf,
                        'loudFloor': floor,
                        'energyThreshold': ethresh,
                        'cooldownMs': 120
                    }
                    result = self.simulate_quiet(params)

                    # Score: prefer hits_per_sec close to target
                    score = abs(result['hits_per_sec'] - target_hps)

                    results.append({
                        'params': params.copy(),
                        'hits_per_sec': result['hits_per_sec'],
                        'avg_sparks': result['avg_sparks'],
                        'score': score
                    })

                    if score < best_score:
                        best_score = score
                        best_params = params.copy()

        # Sort by score
        results.sort(key=lambda x: x['score'])

        print("\nTop 5 configurations:")
        for i, r in enumerate(results[:5]):
            p = r['params']
            print(f"  {i+1}. tfactor={p['transientFactor']}, floor={p['loudFloor']}, "
                  f"energyThresh={p['energyThreshold']} -> {r['hits_per_sec']:.2f} hits/sec")

        print(f"\n*** RECOMMENDED: tfactor={best_params['transientFactor']}, "
              f"floor={best_params['loudFloor']}, energyThresh={best_params['energyThreshold']} ***")

        return best_params

    def simulate_quiet(self, params):
        """Simulate without printing."""
        p = params
        slowAvg = 0.1
        slowAlpha = 0.025
        lastHitIdx = -1000
        cooldownSamples = int(p['cooldownMs'] / 33)

        simulated_hits = []
        simulated_sparks = []

        for i, sample in enumerate(self.data):
            energy = sample['energy']
            slowAvg += slowAlpha * (energy - slowAvg)
            threshold = max(p['loudFloor'], slowAvg * p['transientFactor'])
            cooldownOk = (i - lastHitIdx) > cooldownSamples

            hit = 0
            if cooldownOk and energy > threshold:
                hit = 1.0
                lastHitIdx = i
                simulated_hits.append(i)

            sparks = 1
            if energy > p['energyThreshold']:
                sparks = 2 + int((energy - p['energyThreshold']) * 10)
            if hit > 0.1:
                sparks += 4 + int(hit * 8)
            simulated_sparks.append(sparks)

        return {
            'hits': len(simulated_hits),
            'hits_per_sec': len(simulated_hits) / self.data[-1]['t'] if self.data else 0,
            'avg_sparks': sum(simulated_sparks) / len(simulated_sparks) if simulated_sparks else 0
        }

    def visualize(self):
        """Create ASCII visualization of the recording."""
        if not self.data:
            print("No data to visualize!")
            return

        print("\n" + "="*60)
        print("VISUALIZATION (time-compressed)")
        print("="*60)

        # Compress to ~60 columns
        step = max(1, len(self.data) // 60)

        print("\nEnergy (# = level, * = hit triggered):")
        for i in range(0, len(self.data), step):
            chunk = self.data[i:i+step]
            max_e = max(s['energy'] for s in chunk)
            bar = '#' * int(max_e * 40)
            marker = '*' if any(s['hit'] > 0.9 for s in chunk) else ' '
            print(f"{marker}{bar}")

        print("\nActivity (# = LED brightness):")
        for i in range(0, len(self.data), step):
            chunk = self.data[i:i+step]
            max_a = max(s['activity'] for s in chunk)
            bar = '#' * (max_a // 3)
            print(f" {bar}")

    def timeline(self, output_file='audio_timeline.png'):
        """Create a visual timeline chart with matplotlib."""
        try:
            import matplotlib.pyplot as plt
            import matplotlib.patches as mpatches
        except ImportError:
            print("matplotlib not installed. Run: pip install matplotlib")
            return

        if not self.data:
            print("No data to visualize!")
            return

        # Extract time series
        times = [s['t'] for s in self.data]
        energies = [s['energy'] for s in self.data]
        thresholds = [s.get('threshold', 0.1) for s in self.data]
        hits = [s['hit'] for s in self.data]
        activities = [s['activity'] for s in self.data]
        sparks = [s.get('sparks', 1) for s in self.data]

        # Find hit trigger points (rising edge of hit > 0.9)
        hit_times = []
        for i, h in enumerate(hits):
            if h > 0.9 and (i == 0 or hits[i-1] < 0.5):
                hit_times.append(times[i])

        # Create figure with subplots
        fig, axes = plt.subplots(4, 1, figsize=(14, 10), sharex=True)
        fig.suptitle('Audio-Reactive Fire Timeline', fontsize=14, fontweight='bold')

        # Panel 1: Energy vs Threshold
        ax1 = axes[0]
        ax1.fill_between(times, energies, alpha=0.3, color='orange', label='Energy (mic level)')
        ax1.plot(times, energies, color='orange', linewidth=0.8)
        ax1.plot(times, thresholds, color='red', linewidth=1, linestyle='--', label='Threshold')
        for ht in hit_times:
            ax1.axvline(x=ht, color='green', alpha=0.5, linewidth=2)
        ax1.set_ylabel('Level')
        ax1.set_ylim(0, 1.1)
        ax1.legend(loc='upper right')
        ax1.set_title('Audio Input: Energy vs Threshold (green lines = hit triggers)')
        ax1.grid(True, alpha=0.3)

        # Panel 2: Hit/Transient decay
        ax2 = axes[1]
        ax2.fill_between(times, hits, alpha=0.4, color='green')
        ax2.plot(times, hits, color='green', linewidth=0.8)
        ax2.set_ylabel('Hit')
        ax2.set_ylim(0, 1.1)
        ax2.set_title('Transient Detector Output (decay after trigger)')
        ax2.grid(True, alpha=0.3)

        # Panel 3: Spark count
        ax3 = axes[2]
        ax3.bar(times, sparks, width=(times[1]-times[0])*0.8 if len(times) > 1 else 0.03,
                color='yellow', edgecolor='orange', alpha=0.7)
        ax3.set_ylabel('Sparks')
        ax3.set_ylim(0, max(sparks) * 1.2)
        ax3.set_title('Spark Generation (yellow = burst on hits)')
        ax3.grid(True, alpha=0.3)

        # Panel 4: LED Activity/Brightness
        ax4 = axes[3]
        ax4.fill_between(times, activities, alpha=0.5, color='red')
        ax4.plot(times, activities, color='darkred', linewidth=0.8)
        ax4.set_ylabel('Activity %')
        ax4.set_xlabel('Time (seconds)')
        ax4.set_ylim(0, max(max(activities) * 1.2, 25))
        ax4.set_title('LED Activity (fire brightness)')
        ax4.grid(True, alpha=0.3)

        plt.tight_layout()
        plt.savefig(output_file, dpi=150, bbox_inches='tight')
        print(f"\nTimeline saved to: {output_file}")
        plt.show()


class DeviceController:
    def __init__(self, port=PORT, baud=BAUD):
        self.port = port
        self.baud = baud

    def apply_params(self, params):
        """Send parameters to device."""
        print(f"\nApplying parameters to device...")

        with serial.Serial(self.port, self.baud, timeout=2) as ser:
            time.sleep(0.3)

            commands = [
                f"set tfactor {params['transientFactor']}",
                f"set loudfloor {params['loudFloor']}",
            ]

            for cmd in commands:
                ser.write(f"{cmd}\r\n".encode())
                time.sleep(0.2)
                response = ser.read(ser.in_waiting).decode(errors='ignore').strip()
                print(f"  {cmd} → {response}")

            # Save to flash
            ser.write(b"save\r\n")
            time.sleep(0.5)
            response = ser.read(ser.in_waiting).decode(errors='ignore').strip()
            print(f"  save → {response}")

        print("Done!")

    def get_current_params(self):
        """Read current parameters from device."""
        print("Reading current parameters...")

        with serial.Serial(self.port, self.baud, timeout=2) as ser:
            time.sleep(0.3)
            ser.write(b"show\r\n")
            time.sleep(0.3)
            response = ser.read(ser.in_waiting).decode(errors='ignore')
            print(response)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        print("\nCommands:")
        print("  record   - Record 30 seconds of audio data")
        print("  analyze  - Analyze recording and find optimal params")
        print("  timeline - Create visual timeline chart (requires matplotlib)")
        print("  apply    - Apply recommended parameters to device")
        print("  show     - Show current device parameters")
        print("  live     - Live monitor with current settings")
        return

    cmd = sys.argv[1].lower()

    if cmd == 'record':
        duration = int(sys.argv[2]) if len(sys.argv) > 2 else 30
        recorder = AudioRecorder()
        recorder.record(duration)
        recorder.save()

    elif cmd == 'analyze':
        recorder = AudioRecorder()
        recorder.load()

        analyzer = AudioAnalyzer(recorder.data)
        analyzer.analyze()
        analyzer.visualize()

        print("\n" + "="*60)
        print("PARAMETER TUNING")
        print("="*60)

        # Test current defaults
        print("\nWith current defaults:")
        analyzer.simulate({'transientFactor': 2.0, 'loudFloor': 0.08, 'energyThreshold': 0.4})

        # Find optimal
        best = analyzer.find_optimal()

        # Save recommendation
        with open('recommended_params.json', 'w') as f:
            json.dump(best, f, indent=2)
        print(f"\nSaved recommendation to recommended_params.json")

    elif cmd == 'timeline':
        recorder = AudioRecorder()
        recorder.load()
        analyzer = AudioAnalyzer(recorder.data)
        output = sys.argv[2] if len(sys.argv) > 2 else 'audio_timeline.png'
        analyzer.timeline(output)

    elif cmd == 'apply':
        try:
            with open('recommended_params.json', 'r') as f:
                params = json.load(f)
            print(f"Applying: {params}")
            controller = DeviceController()
            controller.apply_params(params)
        except FileNotFoundError:
            print("No recommended_params.json found. Run 'analyze' first.")

    elif cmd == 'show':
        controller = DeviceController()
        controller.get_current_params()

    elif cmd == 'live':
        duration = int(sys.argv[2]) if len(sys.argv) > 2 else 60
        recorder = AudioRecorder()
        recorder.record(duration)

    else:
        print(f"Unknown command: {cmd}")
        print("Use: record, analyze, apply, show, or live")


if __name__ == '__main__':
    main()
