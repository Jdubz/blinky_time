# Security Policy

## Supported Versions

| Version | Supported          |
| ------- | ------------------ |
| 0.3.x   | :white_check_mark: |
| < 0.3   | :x:                |

## Reporting a Vulnerability

If you discover a security vulnerability in Blinky Time, please report it responsibly:

1. **Do not** open a public GitHub issue for security vulnerabilities
2. Email the maintainers directly or use GitHub's private vulnerability reporting
3. Include:
   - Description of the vulnerability
   - Steps to reproduce
   - Potential impact
   - Suggested fix (if any)

## Security Considerations

### Hardware Security

This is an embedded LED controller project. Security considerations include:

- **No network connectivity**: The main firmware has no WiFi/BLE enabled by default
- **Serial interface**: Debug commands are available via USB serial (physical access required)
- **Flash storage**: Configuration is stored in on-chip flash (not externally accessible)

### Code Security

- No external API calls or network requests in active code
- No user authentication (standalone embedded device)
- Input validation on serial commands to prevent buffer overflows

### Archive Notice

The `archive/` directory contains legacy projects that may include:
- Hardcoded credentials (for development/testing purposes only)
- Deprecated network code

These are **not part of the active firmware** and are retained for historical reference only. Do not use archive code in production.

## Best Practices for Contributors

1. Never commit real credentials, API keys, or secrets
2. Use placeholder values in example configurations
3. Keep sensitive test data in `.gitignore`d files
4. Review code for potential buffer overflows before submitting PRs
