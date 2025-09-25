#!/usr/bin/env python3
"""
Configuration Validator for Blinky Time

Validates device configurations for consistency and correctness.
Prevents configuration errors that could cause hardware damage or poor performance.

Author: Blinky Time Project Contributors
License: Creative Commons Attribution-ShareAlike 4.0 International
"""

import os
import re
import sys
from pathlib import Path

class ConfigValidator:
    def __init__(self):
        self.project_root = Path(__file__).parent.parent
        self.configs_path = self.project_root / "blinky-things" / "configs"
        self.errors = []
        self.warnings = []
    
    def validate_all_configs(self):
        """Validate all device configuration files"""
        print("🔍 Validating Blinky Time configurations...")
        
        # Check that configs directory exists
        if not self.configs_path.exists():
            self.errors.append("configs/ directory not found")
            return False
        
        # Validate each configuration file
        config_files = [
            ("HatConfig.h", 1, "Hat"),
            ("TubeLightConfig.h", 2, "Tube Light"), 
            ("BucketTotemConfig.h", 3, "Bucket Totem")
        ]
        
        for filename, device_type, name in config_files:
            config_path = self.configs_path / filename
            if config_path.exists():
                print(f"   Checking {name} configuration...")
                self.validate_config_file(config_path, device_type, name)
            else:
                self.errors.append(f"Missing configuration file: {filename}")
        
        # Validate main sketch device selection
        self.validate_main_sketch()
        
        return len(self.errors) == 0
    
    def validate_config_file(self, config_path, device_type, name):
        """Validate individual configuration file"""
        try:
            with open(config_path, 'r') as f:
                content = f.read()
            
            # Check for required constants
            required_constants = [
                'LED_COUNT', 'LED_DATA_PIN', 'FIRE_EFFECT_TYPE'
            ]
            
            for constant in required_constants:
                if constant not in content:
                    self.errors.append(f"{name}: Missing constant {constant}")
            
            # Validate LED count ranges
            led_count_match = re.search(r'LED_COUNT\s*=\s*(\d+)', content)
            if led_count_match:
                led_count = int(led_count_match.group(1))
                if led_count < 1 or led_count > 256:
                    self.errors.append(f"{name}: LED_COUNT {led_count} out of range (1-256)")
                elif led_count > 128:
                    self.warnings.append(f"{name}: LED_COUNT {led_count} is high, check power requirements")
            
            # Validate data pin
            data_pin_match = re.search(r'LED_DATA_PIN\s*=\s*(\d+)', content)
            if data_pin_match:
                data_pin = int(data_pin_match.group(1))
                if data_pin != 10:
                    self.warnings.append(f"{name}: LED_DATA_PIN {data_pin} != 10 (standard is D10)")
            
            # Check for color order specification
            if 'NEO_GRB' not in content and 'NEO_RGB' not in content:
                self.errors.append(f"{name}: Missing color order specification (NEO_GRB/NEO_RGB)")
            
            # Validate fire effect parameters
            self.validate_fire_parameters(content, name)
            
            # Device-specific validations
            if device_type == 2:  # Tube Light
                self.validate_tube_light_config(content, name)
            
        except Exception as e:
            self.errors.append(f"{name}: Error reading config file - {e}")
    
    def validate_fire_parameters(self, content, name):
        """Validate fire effect parameters"""
        # Check cooling parameter
        cooling_match = re.search(r'baseCooling\s*=\s*(\d+)', content)
        if cooling_match:
            cooling = int(cooling_match.group(1))
            if cooling < 5 or cooling > 100:
                self.warnings.append(f"{name}: baseCooling {cooling} may cause poor fire effect")
        
        # Check spark chance
        spark_match = re.search(r'sparkChance\s*=\s*([\d.]+)', content)
        if spark_match:
            spark_chance = float(spark_match.group(1))
            if spark_chance < 0.05 or spark_chance > 0.5:
                self.warnings.append(f"{name}: sparkChance {spark_chance} may cause poor fire effect")
    
    def validate_tube_light_config(self, content, name):
        """Validate tube light specific configuration"""
        # Check for zigzag mapping
        if 'zigzag' not in content.lower():
            self.warnings.append(f"{name}: Consider documenting zigzag LED mapping pattern")
        
        # Check matrix dimensions
        if 'MATRIX_WIDTH' in content and 'MATRIX_HEIGHT' in content:
            width_match = re.search(r'MATRIX_WIDTH\s*=\s*(\d+)', content)
            height_match = re.search(r'MATRIX_HEIGHT\s*=\s*(\d+)', content)
            
            if width_match and height_match:
                width = int(width_match.group(1))
                height = int(height_match.group(1))
                expected_leds = width * height
                
                led_count_match = re.search(r'LED_COUNT\s*=\s*(\d+)', content)
                if led_count_match:
                    actual_leds = int(led_count_match.group(1))
                    if actual_leds != expected_leds:
                        self.errors.append(f"{name}: LED_COUNT {actual_leds} != MATRIX dimensions {width}x{height}={expected_leds}")
    
    def validate_main_sketch(self):
        """Validate main sketch device selection"""
        sketch_path = self.project_root / "blinky-things" / "blinky-things.ino"
        
        if not sketch_path.exists():
            self.errors.append("Main sketch blinky-things.ino not found")
            return
        
        try:
            with open(sketch_path, 'r') as f:
                content = f.read()
            
            # Check DEVICE_TYPE definition
            device_type_match = re.search(r'#define\s+DEVICE_TYPE\s+(\d+)', content)
            if device_type_match:
                device_type = int(device_type_match.group(1))
                if device_type < 0 or device_type > 3:
                    self.errors.append(f"DEVICE_TYPE {device_type} out of range (0-3)")
            else:
                self.errors.append("DEVICE_TYPE not defined in main sketch")
            
            # Check for proper includes
            required_includes = [
                '#include "AdaptiveMic.h"',
                '#include "FireEffect.h"', 
                '#include "BatteryMonitor.h"'
            ]
            
            for include in required_includes:
                if include not in content:
                    self.errors.append(f"Missing include: {include}")
            
        except Exception as e:
            self.errors.append(f"Error reading main sketch: {e}")
    
    def print_results(self):
        """Print validation results"""
        print()
        print("=" * 50)
        print("📊 Configuration Validation Results")
        print("=" * 50)
        
        if self.errors:
            print(f"❌ {len(self.errors)} Error(s):")
            for error in self.errors:
                print(f"   • {error}")
        
        if self.warnings:
            print(f"⚠️  {len(self.warnings)} Warning(s):")
            for warning in self.warnings:
                print(f"   • {warning}")
        
        if not self.errors and not self.warnings:
            print("✅ All configurations valid!")
        elif not self.errors:
            print("✅ No critical errors found")
        
        print()
        
        return len(self.errors) == 0

def main():
    validator = ConfigValidator()
    
    success = validator.validate_all_configs()
    validator.print_results()
    
    if success:
        print("🎉 Configuration validation passed!")
        sys.exit(0)
    else:
        print("💥 Configuration validation failed!")
        print("Fix the errors above before proceeding with deployment.")
        sys.exit(1)

if __name__ == "__main__":
    main()