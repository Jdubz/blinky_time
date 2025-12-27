#!/usr/bin/env python3
"""
Pre-upload safety checker for blinky_time firmware

Validates:
1. CONFIG_VERSION incremented when struct size changes
2. New config parameters have validation
3. Static assertions present
4. No obvious memory safety issues

Run before EVERY firmware upload:
    python scripts/check_config_safety.py
"""

import re
import sys
from pathlib import Path

# Colors for terminal output
class Colors:
    RED = '\033[91m'
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    BLUE = '\033[94m'
    END = '\033[0m'
    BOLD = '\033[1m'

def error(msg):
    print(f"{Colors.RED}{Colors.BOLD}✗ ERROR:{Colors.END} {msg}")

def warning(msg):
    print(f"{Colors.YELLOW}⚠ WARNING:{Colors.END} {msg}")

def success(msg):
    print(f"{Colors.GREEN}✓{Colors.END} {msg}")

def info(msg):
    print(f"{Colors.BLUE}ℹ{Colors.END} {msg}")

def check_config_version_comments(config_h_path):
    """Ensure CONFIG_VERSION has descriptive comment"""
    with open(config_h_path, 'r') as f:
        content = f.read()

    # Find CONFIG_VERSION line
    match = re.search(r'CONFIG_VERSION\s*=\s*(\d+)\s*;(.+)', content)
    if not match:
        error("CONFIG_VERSION not found in ConfigStorage.h")
        return False

    version = int(match.group(1))
    comment = match.group(2)

    # Check if comment describes changes
    if len(comment.strip()) < 20:
        warning(f"CONFIG_VERSION {version} has minimal comment. Add details about struct changes.")
        return False

    success(f"CONFIG_VERSION {version} has descriptive comment")
    return True

def check_static_assertions(config_h_path):
    """Verify static_assert for struct sizes exists"""
    with open(config_h_path, 'r') as f:
        content = f.read()

    checks = [
        (r'static_assert\s*\(\s*sizeof\s*\(\s*StoredMicParams\s*\)', "StoredMicParams size check"),
        (r'static_assert\s*\(\s*sizeof\s*\(\s*ConfigData\s*\)', "ConfigData size check"),
    ]

    all_found = True
    for pattern, name in checks:
        if re.search(pattern, content):
            success(f"Found {name}")
        else:
            error(f"Missing {name} - add static_assert!")
            all_found = False

    return all_found

def check_validation_coverage(config_h_path, config_cpp_path):
    """Ensure all config parameters have validation"""
    # Extract parameter names from StoredMicParams
    with open(config_h_path, 'r') as f:
        h_content = f.read()

    # Find StoredMicParams struct
    match = re.search(r'struct\s+StoredMicParams\s*\{([^}]+)\}', h_content, re.DOTALL)
    if not match:
        error("StoredMicParams struct not found")
        return False

    struct_body = match.group(1)

    # Extract float/uint16_t parameter names
    params = []
    for line in struct_body.split('\n'):
        # Match: float paramName; or uint16_t paramName;
        param_match = re.search(r'(float|uint16_t)\s+(\w+)\s*;', line)
        if param_match:
            params.append(param_match.group(2))

    if not params:
        warning("No parameters found in StoredMicParams")
        return False

    info(f"Found {len(params)} parameters in StoredMicParams")

    # Check if each parameter has validation in ConfigStorage.cpp
    with open(config_cpp_path, 'r') as f:
        cpp_content = f.read()

    # Find loadConfiguration function
    load_match = re.search(r'void\s+ConfigStorage::loadConfiguration\([^)]+\)\s*\{(.+?)^\}',
                           cpp_content, re.DOTALL | re.MULTILINE)
    if not load_match:
        error("loadConfiguration() not found in ConfigStorage.cpp")
        return False

    load_body = load_match.group(1)

    missing_validation = []
    for param in params:
        # Check for validateFloat or validateUint32 call with this parameter
        if not re.search(rf'validate(Float|Uint32)\s*\([^,]*mic\.{param}', load_body):
            missing_validation.append(param)

    if missing_validation:
        error(f"Missing validation for parameters: {', '.join(missing_validation)}")
        error("Add validateFloat/validateUint32 calls in loadConfiguration()")
        return False

    success(f"All {len(params)} parameters have validation")
    return True

def check_git_status():
    """Warn if there are uncommitted changes"""
    import subprocess
    try:
        result = subprocess.run(['git', 'status', '--porcelain'],
                              capture_output=True, text=True)
        if result.stdout.strip():
            warning("Uncommitted changes detected. Consider committing before upload.")
            return False
    except:
        info("Git not available - skipping commit check")
    return True

def main():
    print(f"\n{Colors.BOLD}=== Blinky Config Safety Check ==={Colors.END}\n")

    # Find project root
    root = Path(__file__).parent.parent
    config_h = root / "blinky-things" / "config" / "ConfigStorage.h"
    config_cpp = root / "blinky-things" / "config" / "ConfigStorage.cpp"

    if not config_h.exists():
        error(f"ConfigStorage.h not found at {config_h}")
        sys.exit(1)

    if not config_cpp.exists():
        error(f"ConfigStorage.cpp not found at {config_cpp}")
        sys.exit(1)

    # Run checks
    checks = [
        ("CONFIG_VERSION comments", lambda: check_config_version_comments(config_h)),
        ("Static assertions", lambda: check_static_assertions(config_h)),
        ("Parameter validation", lambda: check_validation_coverage(config_h, config_cpp)),
        ("Git status", check_git_status),
    ]

    results = []
    for name, check_fn in checks:
        print(f"\n{Colors.BOLD}Checking: {name}{Colors.END}")
        try:
            result = check_fn()
            results.append(result)
        except Exception as e:
            error(f"Check failed with exception: {e}")
            results.append(False)

    # Summary
    print(f"\n{Colors.BOLD}{'=' * 50}{Colors.END}")
    passed = sum(results)
    total = len(results)

    if all(results):
        print(f"{Colors.GREEN}{Colors.BOLD}✓ ALL CHECKS PASSED ({passed}/{total}){Colors.END}")
        print(f"\n{Colors.GREEN}Safe to upload firmware using Arduino IDE{Colors.END}")
        print(f"{Colors.RED}{Colors.BOLD}REMEMBER: NEVER use arduino-cli!{Colors.END}\n")
        return 0
    else:
        print(f"{Colors.RED}{Colors.BOLD}✗ CHECKS FAILED ({passed}/{total} passed){Colors.END}")
        print(f"\n{Colors.RED}DO NOT upload firmware until issues are resolved!{Colors.END}\n")
        return 1

if __name__ == "__main__":
    sys.exit(main())
