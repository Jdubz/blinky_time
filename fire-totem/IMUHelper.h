#pragma once
#include <Arduino.h>

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };

// Clean IMU sensor data structure
struct IMUData {
    // Raw sensor readings
    Vec3 accel;      // m/s² - raw accelerometer
    Vec3 gyro;       // rad/s - raw gyroscope
    float temp;      // °C - temperature

    // Basic processed data
    Vec3 gravity;    // m/s² - estimated gravity vector
    Vec3 linearAccel; // m/s² - accel with gravity removed

    // Orientation
    Vec3 up;         // unit vector pointing "up" (opposite of gravity)
    float tiltAngle; // degrees from vertical

    // Simple motion indicators
    float motionMagnitude;  // overall motion level
    bool isMoving;         // basic motion detection

    // Timestamp
    unsigned long timestamp; // millis() when data was captured
};

// Legacy motion config - simplified for basic fire effects only
struct MotionConfig {
  // Basic orientation filtering
  float tauLP    = 0.12f;  // s, low-pass for gravity estimate
  float gravityThresh = 5.0f; // m/s² threshold to detect actual motion vs gravity (raised for less sensitivity)

  // Simplified motion parameters (for legacy wind/stoke if still used)
  float kAccel   = 0.1f;   // reduced sensitivity
  float kSpin    = 0.05f;  // reduced sensitivity
  float kStoke   = 0.01f;  // reduced sensitivity
  float maxWindSpeed = 3.0f; // reduced maximum
  float stokeDecay = 0.95f; // faster decay
};

// Legacy motion state - kept for backward compatibility with existing fire effect
// TODO: Migrate fire effect to use IMUData instead
struct MotionState {
  // Basic orientation (still used by fire effect)
  Vec3  up {0,1,0};           // unit vector (world up in torch space)
  float tiltAngle = 0.0f;     // degrees of tilt from vertical

  // Legacy fields - deprecated but kept for compatibility
  Vec3  velocity {0,0,0};     // integrated velocity (m/s)
  Vec3  smoothAccel {0,0,0};  // smoothed acceleration
  Vec2  wind {0,0};           // DEPRECATED: use IMUData instead
  float stoke = 0.0f;         // DEPRECATED: use IMUData instead

  // Rotational motion - may be useful for future effects
  Vec3  angularVel {0,0,0};   // angular velocity (rad/s)
  float spinMagnitude = 0.0f; // overall rotation speed

  // Motion analysis
  float motionIntensity = 0.0f; // overall motion level (0-1)
  bool  isStationary = true;    // true if torch is relatively still
};
class IMUHelper {
public:
    IMUHelper();

    bool begin();
    bool isReady() const { return imuReady; }

    bool getAccel(float &ax, float &ay, float &az);
    bool getGyro(float &gx, float &gy, float &gz);
    float getTempC();

    void        setMotionConfig(const MotionConfig& c) { cfg_ = c; }
    const MotionConfig& getMotionConfig() const { return cfg_; }

    // Call once per frame with dt (seconds).
    // Returns true if sensor data was used this frame.
    bool        updateMotion(float dt);

    // If you prefer to feed raw readings (m/s^2 and rad/s) from elsewhere:
    bool        updateMotionWithRaw(float ax, float ay, float az,
                                  float gx, float gy, float gz,
                                  float dt);

    const MotionState& motion() const { return motion_; }

    // Clean IMU data interface
    const IMUData& getRawIMUData() const { return imuData_; }
    bool updateIMUData(); // Updates imuData_ with fresh sensor readings

private:
    bool imuReady = false;
    // Return false if no fresh data.
    bool readAccelGyro_(float& ax, float& ay, float& az,
                        float& gx, float& gy, float& gz);

    // helpers
    static inline float vdot_(const Vec3&a,const Vec3&b){return a.x*b.x+a.y*b.y+a.z*b.z;}
    static inline float vdot2_(const Vec2&a,const Vec2&b){return a.x*b.x+a.y*b.y;}
    static inline float vlen_(const Vec3&a){return sqrtf(vdot_(a,a));}
    static inline float vlen2_(const Vec2&a){return sqrtf(vdot2_(a,a));}
    static inline Vec3  vnorm_(const Vec3&a){ float L=vlen_(a); return (L>1e-6f)? Vec3{a.x/L,a.y/L,a.z/L} : Vec3{0,1,0}; }
    static inline Vec3  vadd_(const Vec3&a,const Vec3&b){return {a.x+b.x, a.y+b.y, a.z+b.z};}
    static inline Vec3  vsub_(const Vec3&a,const Vec3&b){return {a.x-b.x, a.y-b.y, a.z-b.z};}
    static inline Vec3  vmul_(const Vec3&a,float s){return {a.x*s, a.y*s, a.z*s};}
    static inline Vec3  vcross_(const Vec3&a,const Vec3&b){return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};}
    static inline float radToDeg_(float rad){return rad * 180.0f / M_PI;}
    static inline float degToRad_(float deg){return deg * M_PI / 180.0f;}

    // Simplified physics methods (legacy compatibility)
    void updateBasicOrientation_(const Vec3& accel, float dt);
    void updateSimpleMotion_(const Vec3& accel, const Vec3& gyro, float dt);

    // state
    MotionConfig cfg_;
    MotionState  motion_;
    IMUData      imuData_;         // clean IMU sensor data
    Vec3         gLP_{0,0,9.81f};  // low-pass gravity estimate

    // Basic motion tracking
    Vec3         prevAccel_{0,0,0};     // previous acceleration
    uint32_t     lastUpdateMs_ = 0;     // timing
};
