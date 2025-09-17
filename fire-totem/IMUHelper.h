#pragma once
#include <Arduino.h>

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };

struct MotionConfig {
  float tauLP    = 0.12f;  // s, low-pass for gravity estimate
  float kAccel   = 0.35f;  // lateral accel -> wind
  float kSpin    = 0.06f;  // yaw rate      -> wind
  float kStoke   = 0.18f;  // upward accel  -> stoke
  float stokeMax = 0.80f;  // clamp 0..1
};

struct MotionState {
  Vec3  up {0,1,0};        // unit vector (world up)
  Vec2  wind {0,0};        // lateral “drift” (cells/sec-ish)
  float stoke = 0.0f;      // 0..1 boost when moving upward
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

private:
    bool imuReady = false;
    // Return false if no fresh data.
    bool readAccelGyro_(float& ax, float& ay, float& az,
                        float& gx, float& gy, float& gz);

    // helpers
    static inline float vdot_(const Vec3&a,const Vec3&b){return a.x*b.x+a.y*b.y+a.z*b.z;}
    static inline float vlen_(const Vec3&a){return sqrtf(vdot_(a,a));}
    static inline Vec3  vnorm_(const Vec3&a){ float L=vlen_(a); return (L>1e-6f)? Vec3{a.x/L,a.y/L,a.z/L} : Vec3{0,1,0}; }

    // state
    MotionConfig cfg_;
    MotionState  motion_;
    Vec3         gLP_{0,0,9.81f};  // low-pass gravity estimate
};
