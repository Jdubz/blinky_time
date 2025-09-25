# Contributing to Blinky Time

Thank you for your interest in contributing to Blinky Time! This document provides guidelines for contributing to this open-source LED fire effect controller project.

## 🤝 Code of Conduct

By participating in this project, you agree to maintain a respectful and inclusive environment for all contributors.

## 🚀 Getting Started

### Prerequisites

- Arduino IDE 2.0+ or arduino-cli
- Seeeduino nRF52 Board Package installed
- Access to compatible hardware for testing (recommended)
- Git for version control

### Development Setup

1. **Fork the repository** on GitHub
2. **Clone your fork** locally:
   ```bash
   git clone https://github.com/YOUR_USERNAME/blinky_time.git
   cd blinky_time
   ```
3. **Create a branch** for your changes:
   ```bash
   git checkout -b feature/your-feature-name
   ```
4. **Make your changes** and test thoroughly
5. **Commit and push** your changes
6. **Submit a pull request**

## 📝 Contribution Types

We welcome various types of contributions:

### Code Contributions
- Bug fixes
- New features
- Performance improvements
- Code cleanup and refactoring
- New device configurations

### Documentation
- README improvements
- Code comments and documentation
- Wiki contributions
- Example projects and tutorials

### Testing
- Hardware compatibility testing
- Bug reports with detailed reproduction steps
- Performance testing and optimization

### Design
- Circuit diagrams and schematics
- 3D printable enclosures
- Wiring guides and installation documentation

## 🛠 Development Guidelines

### Code Style

- **Indentation**: 2 spaces (no tabs)
- **Naming**: Use camelCase for variables and functions, PascalCase for classes
- **Comments**: Document complex algorithms and hardware-specific code
- **Line Length**: Keep lines under 100 characters when practical

### File Organization

- **Main sketch**: `blinky-things/blinky-things.ino`
- **Libraries**: Separate `.cpp/.h` files in main directory
- **Configurations**: Device-specific configs in `configs/` directory
- **Documentation**: Additional docs in `docs/` directory

### Commit Messages

Use clear, descriptive commit messages:
```
feat: add support for 8x8 matrix configuration
fix: correct zigzag mapping for tube lights
docs: update installation instructions for Windows
test: add battery monitoring validation
```

## 🧪 Testing

### Hardware Testing

When contributing code changes:

1. **Test on actual hardware** when possible
2. **Verify all device types** if changes affect core functionality
3. **Check battery management** features with real battery usage
4. **Validate audio responsiveness** with various audio inputs

### Testing Checklist

- [ ] Code compiles without warnings
- [ ] Fire effect displays correctly
- [ ] Audio responsiveness works as expected
- [ ] Battery monitoring functions properly
- [ ] Serial console commands respond correctly
- [ ] LED colors and brightness are accurate

## 📋 Pull Request Process

### Before Submitting

1. **Ensure your code compiles** without errors or warnings
2. **Test on hardware** if changes affect device functionality
3. **Update documentation** if you've changed APIs or added features
4. **Write clear commit messages** describing your changes

### Pull Request Template

When submitting a pull request, please include:

```markdown
## Description
Brief description of the changes and their purpose.

## Type of Change
- [ ] Bug fix (non-breaking change that fixes an issue)
- [ ] New feature (non-breaking change that adds functionality)
- [ ] Breaking change (fix or feature that would cause existing functionality to not work as expected)
- [ ] Documentation update

## Hardware Tested
- [ ] Hat Configuration
- [ ] Tube Light Configuration  
- [ ] Bucket Totem Configuration
- [ ] No hardware testing required

## Testing
Describe the tests you ran and their results.

## Screenshots/Videos
If applicable, add visual documentation of your changes.
```

### Review Process

1. **Automated checks** will run on your pull request
2. **Maintainer review** will check code quality and functionality
3. **Hardware testing** may be requested for significant changes
4. **Merge** will occur after approval and successful testing

## 🐛 Bug Reports

### Before Reporting

1. **Search existing issues** to avoid duplicates
2. **Test with latest code** from the main branch
3. **Isolate the problem** to specific conditions

### Bug Report Template

```markdown
**Hardware Configuration**
- Device Type: [Hat/Tube Light/Bucket Totem]
- Board: [nRF52840 XIAO Sense]
- LEDs: [Count and type]
- Power: [Battery/USB/External]

**Software Environment**
- Arduino IDE Version: 
- Library Versions:
- Operating System:

**Description**
Clear description of the bug and expected behavior.

**Steps to Reproduce**
1. Step one
2. Step two
3. Step three

**Additional Context**
Serial output, photos, or videos if helpful.
```

## 💡 Feature Requests

We encourage feature suggestions! Please include:

- **Use case**: Describe the problem you're trying to solve
- **Proposed solution**: Your idea for implementing the feature
- **Hardware requirements**: Any specific hardware needs
- **Alternatives considered**: Other approaches you've thought about

## 📚 Documentation

### Code Documentation

- **Header comments** for all functions explaining purpose and parameters
- **Inline comments** for complex algorithms or hardware-specific code
- **Configuration comments** explaining parameter effects and valid ranges

### Project Documentation

- Keep README.md up to date with new features
- Document hardware setup procedures
- Provide example configurations for new device types
- Update troubleshooting guides with common issues

## 🏷 Versioning

This project follows semantic versioning:
- **Major** (X.0.0): Breaking changes to device configurations or APIs
- **Minor** (0.X.0): New features, device support, or significant improvements
- **Patch** (0.0.X): Bug fixes and minor improvements

## 📄 License

By contributing to this project, you agree that your contributions will be licensed under the Creative Commons Attribution-ShareAlike 4.0 International License.

## 📞 Questions?

- **General questions**: Use GitHub Discussions
- **Bug reports**: Use GitHub Issues
- **Feature requests**: Use GitHub Issues with "enhancement" label

Thank you for contributing to Blinky Time! 🔥