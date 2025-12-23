#!/usr/bin/env python3
"""
Static Initialization Order Fiasco Detector

Scans Arduino/C++ files for dangerous global object declarations that could
cause device bricking due to static initialization order issues.

Usage:
    python scripts/check_static_init.py [directory]
    python scripts/check_static_init.py blinky-things/

The script detects patterns like:
    ClassName instance(arg1, arg2);  // DANGEROUS at global scope
    ClassName instance{arg1, arg2};  // DANGEROUS at global scope (C++11 brace init)
    ClassName instance = ClassName(args);  // DANGEROUS at global scope

Safe patterns (not flagged):
    ClassName* ptr = nullptr;  // Safe - just a pointer
    ClassName instance;  // Safe if default constructor does no hardware access
    static ClassName& get() { static ClassName x; return x; }  // Safe - Meyers singleton (inside function)

Limitations:
    - Uses brace-counting heuristic for scope detection (not a full C++ parser)
    - May have false positives/negatives with complex macros or templates
    - Only checks known dangerous classes listed in DANGEROUS_CLASSES
"""

import re
import sys
from pathlib import Path

# Classes known to be dangerous when constructed globally
# (they access hardware in their constructors)
DANGEROUS_CLASSES = [
    r'Adafruit_NeoPixel',
    r'AdaptiveMic',
    r'BatteryMonitor',
    r'SerialConsole',
    r'Nrf52Gpio',
    r'Nrf52Adc',
    r'Nrf52PdmMic',
    r'ArduinoSystemTime',
    r'NeoPixelLedStrip',
    # Add more as discovered
]

# Build class pattern for regex
CLASS_PATTERN = '|'.join(DANGEROUS_CLASSES)

# Pattern: ClassName instance(args);
DANGEROUS_PATTERN_CTOR = re.compile(
    r'^\s*(?:static\s+)?(' + CLASS_PATTERN + r')\s+(\w+)\s*\([^)]+\)\s*;'
)

# Pattern: ClassName instance{args}; (C++11 brace initialization)
DANGEROUS_PATTERN_BRACE = re.compile(
    r'^\s*(?:static\s+)?(' + CLASS_PATTERN + r')\s+(\w+)\s*\{[^}]+\}\s*;'
)

# Pattern: ClassName instance = ClassName(args);
DANGEROUS_PATTERN_ASSIGN = re.compile(
    r'^\s*(?:static\s+)?(' + CLASS_PATTERN + r')\s+(\w+)\s*=\s*\w+\s*\([^)]+\)\s*;'
)

# Pattern: ClassName instance = ClassName{args}; (C++11 brace)
DANGEROUS_PATTERN_ASSIGN_BRACE = re.compile(
    r'^\s*(?:static\s+)?(' + CLASS_PATTERN + r')\s+(\w+)\s*=\s*\w+\s*\{[^}]+\}\s*;'
)

ALL_DANGEROUS_PATTERNS = [
    DANGEROUS_PATTERN_CTOR,
    DANGEROUS_PATTERN_BRACE,
    DANGEROUS_PATTERN_ASSIGN,
    DANGEROUS_PATTERN_ASSIGN_BRACE,
]

# Safe patterns to ignore (only pointer declarations)
SAFE_PATTERNS = [
    r'\*\s*\w+\s*=\s*nullptr',  # Pointer = nullptr
    r'\*\s*\w+\s*=\s*NULL',     # Pointer = NULL
    r'\*\s*\w+\s*=\s*new\s+',   # Pointer = new (inside function)
]


def is_inside_function(content: str, line_num: int) -> bool:
    """
    Check if a line is inside a function body using brace counting.

    This heuristic counts opening and closing braces up to the given line.
    If there are more opening braces than closing braces, we're likely
    inside a function, class, or namespace body.

    Note: This is more reliable than indentation-based detection but still
    not perfect (doesn't handle braces in strings/comments properly).
    """
    lines = content.split('\n')
    text_before = '\n'.join(lines[:line_num - 1])

    # Remove string literals and comments to avoid counting braces inside them
    # Simple approach: remove // comments and /* */ comments
    text_before = re.sub(r'//.*$', '', text_before, flags=re.MULTILINE)
    text_before = re.sub(r'/\*.*?\*/', '', text_before, flags=re.DOTALL)
    # Remove string literals (simplified - doesn't handle all edge cases)
    text_before = re.sub(r'"[^"\\]*(?:\\.[^"\\]*)*"', '', text_before)

    open_braces = text_before.count('{')
    close_braces = text_before.count('}')

    return open_braces > close_braces


def check_file(filepath: Path) -> list:
    """Check a single file for dangerous static initialization patterns."""
    issues = []

    try:
        content = filepath.read_text(encoding='utf-8', errors='ignore')
    except Exception as e:
        return [f"Error reading {filepath}: {e}"]

    lines = content.split('\n')

    # Check each line for dangerous patterns
    for line_num, line in enumerate(lines, 1):
        stripped = line.lstrip()

        # Skip comments
        if stripped.startswith('//') or stripped.startswith('/*') or stripped.startswith('*'):
            continue

        # Skip if it's a safe pattern (pointer declaration)
        is_safe = False
        for safe in SAFE_PATTERNS:
            if re.search(safe, line):
                is_safe = True
                break
        if is_safe:
            continue

        # Check for dangerous constructor patterns
        for pattern in ALL_DANGEROUS_PATTERNS:
            match = pattern.match(line)
            if match:
                class_name = match.group(1)
                var_name = match.group(2)

                # Use brace counting to determine if we're at global scope
                if not is_inside_function(content, line_num):
                    issues.append({
                        'file': str(filepath),
                        'line': line_num,
                        'class': class_name,
                        'variable': var_name,
                        'code': line.strip()
                    })

    return issues


def check_directory(directory: Path) -> list:
    """Recursively check all C++ files in a directory."""
    all_issues = []
    extensions = {'.ino', '.cpp', '.h', '.hpp', '.c'}

    for filepath in directory.rglob('*'):
        if filepath.suffix.lower() in extensions:
            # Skip mock and test directories for hardware classes
            if 'mock' in str(filepath).lower():
                continue
            issues = check_file(filepath)
            all_issues.extend(issues)

    return all_issues


def main():
    if len(sys.argv) < 2:
        directory = Path('.')
    else:
        directory = Path(sys.argv[1])

    if not directory.exists():
        print(f"Error: Directory '{directory}' not found")
        sys.exit(1)

    print(f"Checking for static initialization issues in: {directory}")
    print("=" * 60)

    issues = check_directory(directory)

    if not issues:
        print("No dangerous static initialization patterns found.")
        print("\nAll global objects appear to use safe patterns:")
        print("  - Pointers initialized to nullptr")
        print("  - Default constructors with begin() methods")
        print("  - Static locals (Meyers singletons inside functions)")
        sys.exit(0)

    print(f"Found {len(issues)} potential static initialization issues:\n")

    for issue in issues:
        if isinstance(issue, str):
            print(issue)
        else:
            print(f"  {issue['file']}:{issue['line']}")
            print(f"    Class: {issue['class']}")
            print(f"    Variable: {issue['variable']}")
            print(f"    Code: {issue['code']}")
            print()

    print("=" * 60)
    print("DANGER: Global objects with constructor arguments can BRICK the device!")
    print()
    print("Fix by using pointers and initializing in setup():")
    print("  BEFORE: ClassName instance(args);")
    print("  AFTER:  ClassName* instance = nullptr;")
    print("          void setup() { instance = new ClassName(args); }")
    print()
    print("See: tests/StaticInitCheck.h for full documentation")

    sys.exit(1)


if __name__ == '__main__':
    main()
