# Blinky Things - Todo List
*Updated: September 23, 2025*

## 🚀 TODO ITEMS

### High Priority - Next Implementation Phase

#### Hardware Verification & Testing
- [ ] **Test Hat configuration on actual XIAO nRF52840 Sense hardware**
  - Verify 89 LED string fire effect  
  - Test STRING_FIRE mode sideways heat dissipation
  - Validate audio responsiveness and IMU orientation
  - Check battery monitoring and charging visualization

- [ ] **Test Tube Light configuration on target hardware**  
  - Verify 4x15 matrix layout and zigzag wiring
  - Test MATRIX_FIRE upward flame propagation
  - Validate vertical orientation detection
  - Check LED mapping for proper visual output

- [ ] **Test Bucket Totem configuration on target hardware**
  - Verify 16x8 matrix standard wiring  
  - Test horizontal fire effect orientation
  - Validate standard totem fire parameters
  - Check overall performance and stability

#### Runtime Configuration Management  
- [ ] **Add serial commands for device configuration switching**
  - `config hat` - Switch to Hat configuration
  - `config tube` - Switch to Tube Light configuration  
  - `config bucket` - Switch to Bucket Totem configuration
  - `config show` - Display current active configuration
  - `config save` - Save current config to EEPROM

- [ ] **Implement EEPROM configuration storage**
  - Persistent storage of custom fire parameters
  - Device type selection persistence across reboots
  - Configuration backup and restore functionality
  - Factory reset capability

### Medium Priority - Enhancements

#### Advanced Configuration Features
- [ ] **Configuration profile system**
  - Multiple named presets per device type
  - Profile switching via serial commands
  - Import/export configuration profiles
  - Preset management interface

- [ ] **Hardware auto-detection system**
  - Detect device type via hardware pins or connected LEDs
  - Automatic configuration selection on startup  
  - Override mechanism for manual selection
  - LED count detection and validation

#### Extended Hardware Support
- [ ] **Additional device configurations**  
  - Support for custom LED counts and arrangements
  - Generic matrix configuration builder
  - LED strip configuration templates
  - Multi-strip synchronized configurations

- [ ] **Enhanced error handling and diagnostics**
  - LED-based error indication patterns
  - Hardware fault detection and reporting
  - Configuration mismatch warnings
  - Performance monitoring and alerts

### Low Priority - Advanced Features

#### Development and Testing Tools
- [ ] **Configuration testing framework**
  - Automated validation of all device configurations
  - Hardware-in-the-loop testing setup  
  - Regression testing for configuration changes
  - Continuous integration build verification

- [ ] **Advanced debugging features**
  - Real-time performance metrics display
  - Memory usage monitoring and optimization
  - Fire effect parameter optimization tools
  - Visual configuration editor interface

#### Future Enhancements
- [ ] **Wireless configuration management**
  - WiFi-based configuration portal
  - Mobile app for parameter tuning
  - Remote monitoring and control
  - Over-the-air configuration updates

- [ ] **Machine learning integration**
  - Adaptive fire parameters based on usage patterns
  - Automatic audio environment detection and optimization
  - User preference learning system
  - Predictive parameter adjustment

## Implementation Priority

### Phase 1: Hardware Validation (1-2 weeks)
Focus on testing all three configurations on actual hardware to ensure the improvements work correctly in real-world scenarios.

### Phase 2: Runtime Configuration (2-3 weeks)  
Implement EEPROM storage and runtime configuration switching to make the system fully dynamic.

### Phase 3: Advanced Features (Ongoing)
Add enhanced features based on user feedback and hardware testing results.

## Success Metrics for Remaining Work

### Hardware Testing Phase
- [ ] All three device types function correctly on hardware
- [ ] Fire effects display properly with expected visual characteristics  
- [ ] Audio responsiveness works as designed
- [ ] IMU orientation detection functions correctly
- [ ] Battery monitoring and charging features operational
- [ ] Serial console commands work reliably

### Runtime Configuration Phase  
- [ ] Device switching works without recompilation
- [ ] Configuration persistence survives power cycles
- [ ] EEPROM storage is reliable and corruption-resistant
- [ ] All configuration parameters can be modified at runtime
- [ ] Factory reset capability restores known-good state

### System Integration
- [ ] Smooth transitions between configurations
- [ ] No memory leaks during configuration changes
- [ ] Stable operation over extended periods
- [ ] Consistent behavior across all supported device types

---

## Current Status Summary

**✅ Major Implementation Complete**: All critical configuration improvements done  
**⏳ Hardware Testing Required**: Need to validate on actual hardware  
**🔄 Runtime Features Next**: EEPROM storage and dynamic configuration switching  
**📈 System Ready**: Foundation is solid for advanced features

The codebase is now in excellent condition with a clean, maintainable architecture that supports easy extension for the remaining todo items.