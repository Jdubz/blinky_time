#!/usr/bin/env python3
"""
Blinky Time Automated Test Runner

Python script to automate compilation and testing of the Blinky Time project.
Supports multiple device configurations and generates test reports.

Usage:
    python run_tests.py [--device TYPE] [--port COM] [--report]
    
Options:
    --device TYPE   Device type to test (1=Hat, 2=Tube, 3=Bucket, all=All)
    --port COM      Serial port for hardware testing
    --report        Generate HTML test report
    --verbose       Verbose output
    --timeout SEC   Serial timeout in seconds (default: 30)

Requirements:
    - Arduino CLI installed and in PATH
    - pyserial for hardware testing
    - Seeeduino nRF52 board package installed

Author: Blinky Time Project Contributors
License: Creative Commons Attribution-ShareAlike 4.0 International
"""

import argparse
import subprocess
import sys
import json
import time
import os
from pathlib import Path
from datetime import datetime

try:
    import serial
    SERIAL_AVAILABLE = True
except ImportError:
    SERIAL_AVAILABLE = False
    print("Warning: pyserial not available. Hardware testing disabled.")

class BlinkyTestRunner:
    def __init__(self):
        self.project_root = Path(__file__).parent.parent
        self.sketch_path = self.project_root / "blinky-things"
        self.test_path = self.project_root / "tests"
        self.results = {
            'timestamp': datetime.now().isoformat(),
            'compilation': [],
            'tests': [],
            'summary': {}
        }
    
    def run_compilation_test(self, device_type):
        """Test compilation for specific device type"""
        print(f"🔨 Testing compilation for device type {device_type}...")
        
        # Modify DEVICE_TYPE in main sketch
        sketch_file = self.sketch_path / "blinky-things.ino"
        self.modify_device_type(sketch_file, device_type)
        
        # Compile
        cmd = [
            "arduino-cli", "compile",
            "--fqbn", "Seeeduino:nrf52:xiaonRF52840Sense",
            str(self.sketch_path)
        ]
        
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
            success = result.returncode == 0
            
            self.results['compilation'].append({
                'device_type': device_type,
                'success': success,
                'output': result.stdout,
                'error': result.stderr
            })
            
            status = "✅ PASS" if success else "❌ FAIL"
            print(f"   Device {device_type}: {status}")
            
            if not success:
                print(f"   Error: {result.stderr}")
                
            return success
            
        except subprocess.TimeoutExpired:
            print(f"   Device {device_type}: ❌ TIMEOUT")
            return False
        except FileNotFoundError:
            print("❌ Arduino CLI not found. Install arduino-cli and add to PATH.")
            return False
    
    def modify_device_type(self, sketch_file, device_type):
        """Modify DEVICE_TYPE in sketch file"""
        with open(sketch_file, 'r') as f:
            content = f.read()
        
        # Replace DEVICE_TYPE definition
        import re
        pattern = r'#define DEVICE_TYPE \d+'
        replacement = f'#define DEVICE_TYPE {device_type}'
        modified_content = re.sub(pattern, replacement, content)
        
        with open(sketch_file, 'w') as f:
            f.write(modified_content)
    
    def run_unit_tests(self, port=None, timeout=30):
        """Run unit tests via serial connection"""
        if not SERIAL_AVAILABLE:
            print("⚠️  Serial library not available. Skipping hardware tests.")
            return False
            
        if not port:
            print("⚠️  No serial port specified. Skipping hardware tests.")
            return False
            
        print(f"🧪 Running unit tests on {port}...")
        
        # First, upload test runner
        test_sketch = self.test_path / "test_runner.ino"
        if not self.upload_sketch(test_sketch, port):
            return False
        
        # Connect to serial and capture test output
        try:
            with serial.Serial(port, 115200, timeout=1) as ser:
                time.sleep(3)  # Wait for board to reset
                
                output_lines = []
                start_time = time.time()
                
                while (time.time() - start_time) < timeout:
                    line = ser.readline().decode('utf-8', errors='ignore').strip()
                    if line:
                        output_lines.append(line)
                        print(f"   {line}")
                        
                        # Check for test completion
                        if "ALL TESTS PASSED!" in line or "SOME TESTS FAILED!" in line:
                            break
                
                # Parse test results
                test_result = self.parse_test_output(output_lines)
                self.results['tests'].append(test_result)
                
                return test_result['success']
                
        except serial.SerialException as e:
            print(f"❌ Serial communication error: {e}")
            return False
    
    def upload_sketch(self, sketch_path, port):
        """Upload sketch to board"""
        cmd = [
            "arduino-cli", "upload",
            "--fqbn", "Seeeduino:nrf52:xiaonRF52840Sense",
            "--port", port,
            str(sketch_path.parent)
        ]
        
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
            return result.returncode == 0
        except:
            return False
    
    def parse_test_output(self, lines):
        """Parse test output and extract results"""
        result = {
            'success': False,
            'total_tests': 0,
            'passed_tests': 0,
            'failed_tests': 0,
            'duration_ms': 0,
            'output': lines
        }
        
        for line in lines:
            if "Total Tests:" in line:
                result['total_tests'] = int(line.split(':')[1].strip())
            elif "Passed:" in line:
                result['passed_tests'] = int(line.split(':')[1].strip())
            elif "Failed:" in line:
                result['failed_tests'] = int(line.split(':')[1].strip())
            elif "Duration:" in line:
                duration_str = line.split(':')[1].strip().replace('ms', '')
                result['duration_ms'] = int(duration_str)
            elif "ALL TESTS PASSED!" in line:
                result['success'] = True
        
        return result
    
    def generate_report(self, filename="test_report.html"):
        """Generate HTML test report"""
        html_template = """
<!DOCTYPE html>
<html>
<head>
    <title>Blinky Time Test Report</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; }
        .pass { color: green; }
        .fail { color: red; }
        .header { background: #f0f0f0; padding: 10px; border-radius: 5px; }
        .section { margin: 20px 0; padding: 15px; border: 1px solid #ddd; border-radius: 5px; }
        pre { background: #f5f5f5; padding: 10px; overflow-x: auto; }
        table { border-collapse: collapse; width: 100%; }
        th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }
        th { background-color: #f2f2f2; }
    </style>
</head>
<body>
    <div class="header">
        <h1>🔥 Blinky Time Test Report</h1>
        <p>Generated: {timestamp}</p>
    </div>
    
    <div class="section">
        <h2>Compilation Tests</h2>
        <table>
            <tr><th>Device Type</th><th>Status</th><th>Details</th></tr>
            {compilation_rows}
        </table>
    </div>
    
    <div class="section">
        <h2>Unit Tests</h2>
        {unit_test_section}
    </div>
    
    <div class="section">
        <h2>Test Summary</h2>
        <p>Overall Status: <span class="{overall_class}">{overall_status}</span></p>
    </div>
</body>
</html>
"""
        
        # Generate compilation rows
        comp_rows = []
        for comp in self.results['compilation']:
            status_class = 'pass' if comp['success'] else 'fail'
            status_text = 'PASS' if comp['success'] else 'FAIL'
            comp_rows.append(f"<tr><td>Device {comp['device_type']}</td><td class='{status_class}'>{status_text}</td><td>{comp.get('error', 'OK')}</td></tr>")
        
        # Generate unit test section
        unit_section = ""
        if self.results['tests']:
            test = self.results['tests'][0]  # Assume one test run
            status_class = 'pass' if test['success'] else 'fail'
            unit_section = f"""
            <p>Tests Run: {test['total_tests']}</p>
            <p>Passed: <span class="pass">{test['passed_tests']}</span></p>
            <p>Failed: <span class="fail">{test['failed_tests']}</span></p>
            <p>Duration: {test['duration_ms']}ms</p>
            """
        else:
            unit_section = "<p>No unit tests run</p>"
        
        # Overall status
        all_comp_passed = all(c['success'] for c in self.results['compilation'])
        all_tests_passed = len(self.results['tests']) == 0 or all(t['success'] for t in self.results['tests'])
        overall_success = all_comp_passed and all_tests_passed
        overall_status = "ALL TESTS PASSED" if overall_success else "SOME TESTS FAILED"
        overall_class = "pass" if overall_success else "fail"
        
        html_content = html_template.format(
            timestamp=self.results['timestamp'],
            compilation_rows='\n'.join(comp_rows),
            unit_test_section=unit_section,
            overall_status=overall_status,
            overall_class=overall_class
        )
        
        with open(filename, 'w') as f:
            f.write(html_content)
        
        print(f"📊 Test report generated: {filename}")

def main():
    parser = argparse.ArgumentParser(description='Blinky Time Test Runner')
    parser.add_argument('--device', default='all', help='Device type to test (1,2,3,all)')
    parser.add_argument('--port', help='Serial port for hardware testing')
    parser.add_argument('--report', action='store_true', help='Generate HTML report')
    parser.add_argument('--verbose', action='store_true', help='Verbose output')
    parser.add_argument('--timeout', type=int, default=30, help='Serial timeout')
    
    args = parser.parse_args()
    
    runner = BlinkyTestRunner()
    
    print("🔥 Blinky Time Automated Test Runner")
    print("=" * 40)
    
    # Compilation tests
    device_types = [1, 2, 3] if args.device == 'all' else [int(args.device)]
    
    compilation_success = True
    for device_type in device_types:
        if not runner.run_compilation_test(device_type):
            compilation_success = False
    
    # Unit tests (if port specified)
    unit_test_success = True
    if args.port:
        unit_test_success = runner.run_unit_tests(args.port, args.timeout)
    
    # Generate report
    if args.report:
        runner.generate_report()
    
    # Final status
    overall_success = compilation_success and unit_test_success
    print("\n" + "=" * 40)
    if overall_success:
        print("✅ ALL TESTS PASSED! Safe to refactor.")
        sys.exit(0)
    else:
        print("❌ SOME TESTS FAILED! Fix before refactoring.")
        sys.exit(1)

if __name__ == "__main__":
    main()