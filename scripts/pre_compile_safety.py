#!/usr/bin/env python3
"""
Pre-compile safety gate for Blinky Time firmware.

Runs all safety checks before compilation. Critical checks block the build;
warning checks report issues but allow compilation to proceed.

Usage:
    python3 scripts/pre_compile_safety.py

Exit codes:
    0 - All critical checks passed
    1 - One or more critical checks failed (build MUST be blocked)
"""

import subprocess
import sys
from pathlib import Path

# ANSI colors
RED = '\033[91m'
GREEN = '\033[92m'
YELLOW = '\033[93m'
BOLD = '\033[1m'
RESET = '\033[0m'

ROOT = Path(__file__).parent.parent


def run_check(name, cmd, critical=True):
    """Run a safety check script and return (passed, output)."""
    label = "CRITICAL" if critical else "WARNING"
    print(f"\n{BOLD}[{label}] {name}{RESET}")
    print(f"  Running: {' '.join(cmd)}")

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=30,
            cwd=ROOT,
        )
        output = (result.stdout + result.stderr).strip()
        if output:
            for line in output.split('\n'):
                print(f"  {line}")
        passed = result.returncode == 0
    except FileNotFoundError:
        print(f"  {YELLOW}Skipped (command not found){RESET}")
        # Missing warning tools are not failures; missing critical tools are
        passed = not critical
        output = "command not found"
    except subprocess.TimeoutExpired:
        print(f"  {YELLOW}Timed out after 30s{RESET}")
        passed = not critical
        output = "timed out"

    status = f"{GREEN}PASSED{RESET}" if passed else (f"{RED}FAILED{RESET}" if critical else f"{YELLOW}WARNINGS{RESET}")
    print(f"  Result: {status}")
    return passed


def main():
    print(f"{BOLD}{'=' * 60}")
    print("Blinky Time Pre-Compile Safety Gate")
    print(f"{'=' * 60}{RESET}")

    critical_results = []
    warning_results = []

    # --- Critical checks (block build on failure) ---

    critical_results.append(run_check(
        "Static Initialization Order Fiasco (SIOF) detection",
        [sys.executable, "scripts/check_static_init.py", "blinky-things/"],
        critical=True,
    ))

    critical_results.append(run_check(
        "Config safety (CONFIG_VERSION, static assertions, validation)",
        [sys.executable, "scripts/check_config_safety.py"],
        critical=True,
    ))

    # --- Warning checks (report but don't block) ---

    warning_results.append(run_check(
        "Device configuration validation",
        [sys.executable, "tests/validate_configs.py"],
        critical=False,
    ))

    warning_results.append(run_check(
        "Syntax validation (brace/paren balance)",
        [sys.executable, "tools/validate_syntax.py", "blinky-things/"],
        critical=False,
    ))

    warning_results.append(run_check(
        "Silent fallback anti-pattern detection",
        [sys.executable, "scripts/check_silent_fallbacks.py"],
        critical=False,
    ))

    # cppcheck is optional — skip silently if not installed
    warning_results.append(run_check(
        "cppcheck static analysis",
        ["cppcheck", "--error-exitcode=1", "--quiet",
         "--suppress=missingInclude", "--suppress=unusedFunction",
         "blinky-things/"],
        critical=False,
    ))

    # --- Summary ---
    critical_passed = sum(critical_results)
    critical_total = len(critical_results)
    warning_passed = sum(warning_results)
    warning_total = len(warning_results)

    print(f"\n{BOLD}{'=' * 60}")
    print(f"Safety Gate Summary")
    print(f"{'=' * 60}{RESET}")
    print(f"  Critical: {critical_passed}/{critical_total} passed")
    print(f"  Warnings: {warning_passed}/{warning_total} passed")

    if all(critical_results):
        print(f"\n{GREEN}{BOLD}Safety gate PASSED — compilation may proceed{RESET}")
        return 0
    else:
        print(f"\n{RED}{BOLD}Safety gate FAILED — compilation BLOCKED{RESET}")
        print(f"{RED}Fix critical errors above before uploading firmware.{RESET}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
