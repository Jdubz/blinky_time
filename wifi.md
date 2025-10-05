# WiFi Alternative: BLE Mesh Network Project Plan

## Project Overview

This document outlines a comprehensive project plan for implementing a low-latency mesh network using Seeed Studio XIAO nRF52840 Sense Arduino boards as an alternative to WiFi for controlling WS2812 LEDs with spatial localization capabilities.

## Problem Statement

The original plan to use XIAO nRF52840 Sense boards for WiFi connectivity was not feasible because these boards lack WiFi capability (they only support BLE). This project plan provides a robust alternative using BLE mesh networking that achieves the core requirements:

- Low-latency event distribution
- LED control coordination
- Spatial device localization
- Fault-tolerant communication

## Technical Architecture

### Hardware Components

#### Primary Devices (Tags)
- **Board**: Seeed Studio XIAO nRF52840 Sense
- **Quantity**: 10 stationary devices
- **Function**: LED control nodes with BLE beaconing
- **Features**: 
  - WS2812 LED strip control
  - BLE advertising for mesh communication
  - Optional: Microphone/IMU integration

#### Anchor Devices (Scanners)
- **Board**: 4-6 additional nRF52840 boards
- **Placement**: Room corners + optional mid-wall positions
- **Height**: ~1.5m above floor
- **Function**: RSSI scanning and data forwarding to central controller

#### Central Controller
- **Hardware**: Raspberry Pi 4 (Ubuntu Server)
- **Function**: Location mapping, mesh coordination, heavy computation
- **Connectivity**: USB serial to anchors

#### LED Hardware
- **Type**: WS2812B addressable LED strips
- **Power**: 5V external supply with proper grounding
- **Signal**: 3.3V → 5V level shifting required
- **Protection**: 330-470Ω series resistor, 1000µF capacitor

### Room Configuration

**Dimensions**: 20ft × 10ft room
**Device Layout**: 10 randomly positioned stationary devices
**Anchor Placement**: 4-6 corner/wall-mounted scanning nodes
**Expected Accuracy**: 0.5-1.5m with fingerprinting, 1-3m with ranging

## Communication Protocols

### BLE Advertising Flood Mesh

#### Packet Structure (10 bytes)
```
[NET_ID(1) | TYPE(1) | SRC(2) | SEQ(2) | PAYLOAD(2) | TTL(1) | CRC8(1)]
```

#### Timing Parameters
- **Advertising Interval**: 20-30ms during active rebroadcast
- **Idle Interval**: 200-500ms during normal operation
- **Relay Backoff**: Random 3-12ms before forwarding
- **Scanner Duty**: Near-continuous (40ms interval/window)

#### Performance Characteristics
- **Single Hop Latency**: ~20-40ms median
- **3-Hop Latency**: ~60-150ms typical
- **Reliability**: High due to flooding and redundancy

### Location System Architecture

#### Tag Beacons
```cpp
struct LocationPayload {
    uint8_t net_id;
    uint16_t tag_id;
    int8_t tx_power_dbm;
    uint16_t sequence;
} __attribute__((packed));
```

#### Anchor Data Stream
```
Format: timestamp,anchor_id,tag_id,rssi,channel
Example: 1234567,A01,0x1234,-45,37
```

## Implementation Phases

### Phase 1: Basic BLE Mesh (Weeks 1-2)

#### Objectives
- Implement basic flooding mesh protocol
- Establish reliable event distribution
- Test LED synchronization

#### Deliverables
1. **Tag Firmware** (Arduino)
   - BLE advertising with custom packet format
   - Duplicate detection and relay logic
   - Basic LED pattern control

2. **Mesh Library**
   - Packet serialization/deserialization
   - TTL management and flood control
   - Sequence number tracking

3. **Basic Testing**
   - 2-3 node mesh validation
   - Latency measurements
   - Reliability testing

### Phase 2: LED Integration (Weeks 3-4)

#### Objectives
- Integrate WS2812 LED control
- Implement proper electrical interfacing
- Optimize timing for mesh + LED coordination

#### Deliverables
1. **Hardware Interface**
   - 74AHCT125 level shifters
   - Power supply design
   - Grounding and protection circuits

2. **Software Integration**
   - Non-blocking LED updates
   - Mesh message queuing
   - Interrupt timing optimization

3. **Effect Coordination**
   - Synchronized light patterns
   - Event-driven animations
   - Brightness and timing controls

### Phase 3: Spatial Localization (Weeks 5-7)

#### Objectives
- Implement RSSI-based ranging
- Deploy anchor scanning infrastructure
- Develop location mapping algorithms

#### Deliverables
1. **Anchor Firmware**
   - Continuous BLE scanning
   - RSSI data collection and filtering
   - USB serial data forwarding

2. **Raspberry Pi Software**
   - Multi-anchor data aggregation
   - RSSI filtering and processing
   - Real-time location estimation

3. **Localization Algorithms**
   - Multilateration with robust loss functions
   - Fingerprinting with calibration grid
   - Kalman filtering for movement tracking

### Phase 4: System Integration (Weeks 8-9)

#### Objectives
- Combine mesh networking with location services
- Implement spatial-aware LED effects
- Performance optimization and tuning

#### Deliverables
1. **Integrated Firmware**
   - Combined mesh + localization beaconing
   - Location-aware effect triggers
   - Dynamic mesh topology adjustment

2. **Central Control System**
   - Web-based monitoring interface
   - Real-time device mapping
   - Effect coordination dashboard

3. **Calibration Tools**
   - Automated anchor calibration
   - Path-loss model optimization
   - Room-specific tuning utilities

### Phase 5: OTA and Production Features (Weeks 10-11)

#### Objectives
- Implement over-the-air updates
- Add security and authentication
- Finalize production-ready features

#### Deliverables
1. **OTA Implementation**
   - BLE DFU bootloader integration
   - Secure image signing and verification
   - Remote update coordination

2. **Security Features**
   - Shared key authentication
   - Replay attack prevention
   - Access control mechanisms

3. **Production Tools**
   - Device provisioning system
   - Diagnostic and monitoring tools
   - Documentation and user guides

## Technical Specifications

### Power Requirements
- **LED Budget**: 60mA per LED (worst case white)
- **Typical Usage**: 20-40% brightness for thermal management
- **Power Injection**: Every 50-100 LEDs for longer strips
- **MCU Power**: ~20mA active, optimizable with sleep modes

### Timing Constraints
- **WS2812 Critical Section**: ≤400µs for small strips
- **LED Frame Rate**: 30-60 FPS maximum
- **Mesh Message Rate**: 10Hz beacon, 100Hz burst during events
- **Location Update Rate**: 1-5Hz for stationary devices

### Memory Footprint
- **Seen Packet Cache**: 64-entry ring buffer (~256 bytes)
- **Mesh Queue**: 8-16 message slots (~160 bytes)
- **Location Buffer**: 20-50 RSSI samples per anchor (~400 bytes)
- **Total RAM**: <2KB additional over base LED firmware

## Risk Mitigation

### Technical Risks

1. **RSSI Variability**
   - **Risk**: Indoor multipath affecting location accuracy
   - **Mitigation**: Extensive filtering, multiple calibration points
   - **Fallback**: Proximity zones instead of precise coordinates

2. **Mesh Scalability**
   - **Risk**: Flooding congestion with >10 nodes
   - **Mitigation**: Adaptive TTL, rate limiting, channel diversity
   - **Fallback**: Hybrid star-mesh topology

3. **LED Timing Interference**
   - **Risk**: WS2812 interrupts affecting BLE reception
   - **Mitigation**: Double-buffering, frame rate limiting
   - **Fallback**: Dedicated scanner nodes

### Implementation Risks

1. **Hardware Integration**
   - **Risk**: Level shifting and power supply issues
   - **Mitigation**: Proven reference designs, extensive testing
   - **Fallback**: 3.7V-4.2V LED operation

2. **Arduino Library Compatibility**
   - **Risk**: Adafruit nRF52 core limitations
   - **Mitigation**: Nordic SDK fallback option
   - **Fallback**: PlatformIO with native SDK

## Success Metrics

### Performance Targets
- **Mesh Latency**: <100ms for 90% of single-hop messages
- **Location Accuracy**: <1.5m for 80% of stationary readings
- **LED Synchronization**: <50ms variance across all nodes
- **Reliability**: >95% message delivery in typical conditions

### Functional Requirements
- **Device Discovery**: Automatic mesh joining within 30 seconds
- **Fault Tolerance**: Network recovery from 50% node failures
- **OTA Updates**: Complete network update within 10 minutes
- **Battery Life**: >8 hours continuous operation (if applicable)

## Development Resources

### Required Skills
- Arduino/embedded C++ programming
- BLE protocol implementation
- Signal processing and filtering
- Python for Raspberry Pi integration
- Hardware integration and debugging

### Development Tools
- **IDE**: Arduino IDE 2.0+ or PlatformIO
- **Debug**: Nordic nRF Connect, logic analyzer
- **Testing**: RF chamber (optional), RSSI mapping tools
- **Version Control**: Git with submodule for libraries

### Hardware Requirements
- **Prototyping**: Breadboards, logic analyzer, oscilloscope
- **Production**: Custom PCB design (optional)
- **Test Equipment**: RF power meter, spectrum analyzer (nice to have)

## Timeline Summary

| Phase | Duration | Key Milestones |
|-------|----------|----------------|
| 1 | Weeks 1-2 | Basic mesh operational |
| 2 | Weeks 3-4 | LED integration complete |
| 3 | Weeks 5-7 | Location system functional |
| 4 | Weeks 8-9 | Full system integration |
| 5 | Weeks 10-11 | Production features |

**Total Project Duration**: 11 weeks
**Critical Path**: Mesh protocol → LED integration → Location accuracy
**Key Dependencies**: Hardware procurement, room access for calibration

## Future Enhancements

### Potential Upgrades
1. **Ultra-Wideband (UWB)** integration for sub-meter accuracy
2. **Bluetooth Direction Finding** with nRF5340 and antenna arrays
3. **Thread/Matter** integration for IP connectivity
4. **Machine Learning** for advanced location prediction
5. **Audio Analysis** using integrated microphones for sound localization

### Scalability Options
- Support for mobile devices
- Multi-room coordination
- Integration with existing smart home systems
- Cloud connectivity for remote monitoring

---

*This project plan provides a comprehensive roadmap for implementing a WiFi alternative using BLE mesh networking, achieving low-latency LED control with spatial localization capabilities.*