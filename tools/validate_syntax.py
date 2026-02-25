#!/usr/bin/env python3
"""
Simple syntax validation for Arduino C++ code
Checks for common issues that would cause compilation failures
"""

import os
import re
import sys
from pathlib import Path

def check_file_syntax(filepath):
    """Check a single file for common syntax issues"""
    issues = []
    
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            content = f.read()
            
        lines = content.split('\n')
        
        # Check for common issues
        brace_count = 0
        paren_count = 0
        
        for i, line in enumerate(lines, 1):
            stripped = line.strip()
            
            # Skip comments and empty lines
            if stripped.startswith('//') or stripped.startswith('/*') or not stripped:
                continue
                
            # Count braces and parentheses
            brace_count += line.count('{') - line.count('}')
            paren_count += line.count('(') - line.count(')')
            
            # Check for missing semicolons on non-preprocessor lines
            if (not stripped.startswith('#') and 
                not stripped.endswith('{') and 
                not stripped.endswith('}') and
                not stripped.endswith(',') and
                not stripped.endswith(';') and
                not stripped.endswith('*/') and
                not ('(' in stripped and ')' in stripped) and
                len(stripped) > 0):
                # This might be missing a semicolon
                if not any(keyword in stripped for keyword in ['if', 'for', 'while', 'switch', 'class', 'struct', 'enum']):
                    issues.append(f"Line {i}: Possible missing semicolon: {stripped}")
        
        # Check final brace/paren balance
        if brace_count != 0:
            issues.append(f"Unbalanced braces: {brace_count} extra opening braces")
        if paren_count != 0:
            issues.append(f"Unbalanced parentheses: {paren_count} extra opening parentheses")
            
        # Check for undefined functions being called
        function_calls = re.findall(r'(\w+)\s*\(', content)
        function_definitions = re.findall(r'(?:void|int|bool|float|double|char|String|\w+\*?)\s+(\w+)\s*\([^)]*\)\s*{', content)
        
        undefined_calls = set(function_calls) - set(function_definitions)
        # Filter out common Arduino/C++ functions
        common_functions = {
            'Serial', 'pinMode', 'digitalWrite', 'digitalRead', 'analogRead', 'analogWrite',
            'delay', 'millis', 'micros', 'setup', 'loop', 'print', 'println', 'begin',
            'printf', 'sprintf', 'strlen', 'strcpy', 'strcmp', 'malloc', 'free',
            'new', 'delete', 'sizeof', 'memcpy', 'memset', 'F'
        }
        
        for call in undefined_calls:
            if call not in common_functions and not call[0].isupper():  # Skip constructors
                issues.append(f"Possible undefined function call: {call}")
                
    except Exception as e:
        issues.append(f"Error reading file: {e}")
        
    return issues

def validate_arduino_project(project_path):
    """Validate all C++ files in an Arduino project"""
    project_path = Path(project_path)
    cpp_files = list(project_path.glob('**/*.cpp')) + list(project_path.glob('**/*.ino')) + list(project_path.glob('**/*.h'))
    
    all_issues = {}
    
    for cpp_file in cpp_files:
        if 'archive' in str(cpp_file):  # Skip archive folder
            continue
            
        issues = check_file_syntax(cpp_file)
        if issues:
            all_issues[str(cpp_file)] = issues
    
    return all_issues

if __name__ == "__main__":
    if len(sys.argv) > 1:
        project_path = sys.argv[1]
    else:
        project_path = "."
    
    print(f"Validating Arduino project at: {project_path}")
    print("=" * 50)
    
    issues = validate_arduino_project(project_path)
    
    if not issues:
        print("✅ No obvious syntax issues found!")
    else:
        print("⚠️  Potential issues found:")
        for file_path, file_issues in issues.items():
            print(f"\n📁 {file_path}:")
            for issue in file_issues:
                print(f"  - {issue}")
    
    if issues:
        sys.exit(1)

    print("\nNote: This is a basic syntax check. Compilation may still fail due to:")
    print("- Missing library dependencies")
    print("- Platform-specific issues")
    print("- Complex template/macro expansions")
    print("- Linker issues")