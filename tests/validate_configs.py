#!/usr/bin/env python3
"""
Configuration Validator for Blinky Time

Validates device configurations for consistency and correctness.
Checks DeviceConfig struct fields (.matrix.width, .matrix.ledPin, etc.)
against expected ranges and validates the main sketch includes.

Author: Blinky Time Project Contributors
License: Creative Commons Attribution-ShareAlike 4.0 International
"""

import re
import sys
from pathlib import Path


class ConfigValidator:
    def __init__(self):
        self.project_root = Path(__file__).parent.parent
        self.devices_path = self.project_root / "blinky-things" / "devices"
        self.errors = []
        self.warnings = []

    def validate_all_configs(self):
        """Validate all device configuration files"""
        print("Validating Blinky Time device configurations...")

        if not self.devices_path.exists():
            self.errors.append(f"devices/ directory not found at {self.devices_path}")
            return False

        config_files = [
            ("HatConfig.h", "Hat"),
            ("TubeLightConfig.h", "Tube Light"),
            ("BucketTotemConfig.h", "Bucket Totem"),
            ("LongTubeConfig.h", "Long Tube"),
        ]

        for filename, name in config_files:
            config_path = self.devices_path / filename
            if config_path.exists():
                print(f"   Checking {name} configuration...")
                self.validate_config_file(config_path, name)
            else:
                self.errors.append(f"Missing configuration file: {filename}")

        # Validate DeviceConfig.h struct exists
        device_config_h = self.devices_path / "DeviceConfig.h"
        if not device_config_h.exists():
            self.errors.append("Missing DeviceConfig.h struct definition")

        # Validate main sketch
        self.validate_main_sketch()

        return len(self.errors) == 0

    def validate_config_file(self, config_path, name):
        """Validate individual device configuration file"""
        try:
            content = config_path.read_text()
        except Exception as e:
            self.errors.append(f"{name}: Error reading config file - {e}")
            return

        # Must include DeviceConfig.h
        if '#include "DeviceConfig.h"' not in content:
            self.errors.append(f"{name}: Missing #include \"DeviceConfig.h\"")

        # Must define a DeviceConfig const
        if "const DeviceConfig" not in content:
            self.errors.append(f"{name}: Missing DeviceConfig definition")

        # Check .matrix.width
        width_match = re.search(r'\.width\s*=\s*(\d+)', content)
        if width_match:
            width = int(width_match.group(1))
            if width < 1 or width > 256:
                self.errors.append(f"{name}: matrix.width {width} out of range (1-256)")
        else:
            self.errors.append(f"{name}: Missing .matrix.width")

        # Check .matrix.height
        height_match = re.search(r'\.height\s*=\s*(\d+)', content)
        if height_match:
            height = int(height_match.group(1))
            if height < 1 or height > 256:
                self.errors.append(f"{name}: matrix.height {height} out of range (1-256)")
        else:
            self.errors.append(f"{name}: Missing .matrix.height")

        # Validate total LED count (width * height)
        if width_match and height_match:
            total = int(width_match.group(1)) * int(height_match.group(1))
            if total > 512:
                self.warnings.append(f"{name}: {total} total LEDs is high, check power requirements")

        # Check .matrix.ledPin
        pin_match = re.search(r'\.ledPin\s*=\s*(\w+)', content)
        if pin_match:
            pin = pin_match.group(1)
            if pin not in ('D0', 'D10', 'D1', 'D2', 'D3', 'D4', 'D5', 'D6', 'D7', 'D8', 'D9'):
                self.warnings.append(f"{name}: ledPin {pin} is unusual (expected D0-D10)")
        else:
            self.errors.append(f"{name}: Missing .matrix.ledPin")

        # Check .matrix.ledType includes NEO_GRB or NEO_RGB
        if 'NEO_GRB' not in content and 'NEO_RGB' not in content:
            self.errors.append(f"{name}: Missing color order (NEO_GRB/NEO_RGB) in ledType")

        # Check .matrix.brightness
        brightness_match = re.search(r'\.brightness\s*=\s*(\d+)', content)
        if brightness_match:
            brightness = int(brightness_match.group(1))
            if brightness < 1 or brightness > 255:
                self.errors.append(f"{name}: brightness {brightness} out of range (1-255)")

        # Validate fire defaults if present
        self.validate_fire_defaults(content, name)

    def validate_fire_defaults(self, content, name):
        """Validate fire effect default parameters"""
        cooling_match = re.search(r'\.baseCooling\s*=\s*(\d+)', content)
        if cooling_match:
            cooling = int(cooling_match.group(1))
            if cooling < 5 or cooling > 100:
                self.warnings.append(f"{name}: baseCooling {cooling} may cause poor fire effect")

        spark_match = re.search(r'\.sparkChance\s*=\s*([\d.]+)', content)
        if spark_match:
            spark_chance = float(spark_match.group(1))
            if spark_chance < 0.01 or spark_chance > 0.8:
                self.warnings.append(f"{name}: sparkChance {spark_chance} out of typical range")

    def validate_main_sketch(self):
        """Validate main sketch includes and structure"""
        sketch_path = self.project_root / "blinky-things" / "blinky-things.ino"

        if not sketch_path.exists():
            self.errors.append("Main sketch blinky-things.ino not found")
            return

        try:
            content = sketch_path.read_text()
        except Exception as e:
            self.errors.append(f"Error reading main sketch: {e}")
            return

        # Check for BlinkyArchitecture.h (current architecture include)
        if 'BlinkyArchitecture.h' not in content:
            self.errors.append("Missing include: BlinkyArchitecture.h (current architecture)")

    def print_results(self):
        """Print validation results"""
        print()
        print("=" * 50)
        print("Configuration Validation Results")
        print("=" * 50)

        if self.errors:
            print(f"  {len(self.errors)} Error(s):")
            for error in self.errors:
                print(f"   - {error}")

        if self.warnings:
            print(f"  {len(self.warnings)} Warning(s):")
            for warning in self.warnings:
                print(f"   - {warning}")

        if not self.errors and not self.warnings:
            print("  All configurations valid!")
        elif not self.errors:
            print("  No critical errors found")

        print()
        return len(self.errors) == 0


def main():
    validator = ConfigValidator()

    success = validator.validate_all_configs()
    validator.print_results()

    if success:
        print("Configuration validation passed!")
        sys.exit(0)
    else:
        print("Configuration validation failed!")
        print("Fix the errors above before proceeding.")
        sys.exit(1)


if __name__ == "__main__":
    main()
