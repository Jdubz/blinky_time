#pragma once
#include <Arduino.h>

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };

struct MotionConfig {
  // Gravity and orientation
  float tauLP    = 0.12f;  // s, low-pass for gravity estimate
  float gravityThresh = 0.5f; // m/s² threshold to detect actual motion vs gravity

  // Torch physics constants (calibrated for 1-inch pixels, cylindrical 16x8 matrix)
  float torchLength = 8.0f;      // inches, height of cylinder
  float torchRadius = 2.55f;     // inches, radius (16 pixels * 1" / 2π ≈ 2.55")
  float airDensity  = 1.225f;    // kg/m³, standard air density
  float flameInertia = 0.3f;     // flame response lag factor

  // Wind simulation
  float kAccel   = 0.8f;   // lateral accel -> wind (increased for responsiveness)
  float kSpin    = 0.15f;  // yaw rate -> wind (increased for rotation effects)
  float kVelocity = 0.4f;  // integrated velocity -> wind drift
  float windDamping = 0.92f; // per-frame wind decay (higher = more persistent)
  float maxWindSpeed = 12.0f; // pixels/sec maximum wind effect

  // Rotational effects
  float centrifugalFactor = 0.25f; // how much rotation spreads flames outward
  float coriolisFactor = 0.1f;     // how much rotation creates flame spiral
  float spinDamping = 0.88f;       // rotational momentum decay

  // Stoking (upward motion)
  float kStoke   = 0.35f;  // upward accel -> stoke (increased sensitivity)
  float stokeMax = 1.0f;   // clamp 0..1 (allow full intensity)
  float stokeDecay = 0.85f; // how quickly stoke effect fades
  float stokeVelocityFactor = 0.2f; // upward velocity also contributes to stoke

  // Motion dampening for smooth visuals
  float jerkLimit = 15.0f;      // m/s³ maximum jerk (change in acceleration)
  float smoothingFactor = 0.7f; // motion smoothing (0=raw, 1=heavily filtered)

  // Torch orientation effects
  float tiltSensitivity = 1.5f;    // how much tilt affects flame direction
  float tiltMaxAngle = 45.0f;      // degrees, maximum tilt before flame direction changes
};

struct MotionState {
  // Orientation and gravity
  Vec3  up {0,1,0};           // unit vector (world up in torch space)
  Vec3  torchAxis {0,1,0};    // torch orientation axis
  float tiltAngle = 0.0f;     // degrees of tilt from vertical

  // Linear motion
  Vec3  velocity {0,0,0};     // integrated velocity (m/s)
  Vec3  smoothAccel {0,0,0};  // smoothed acceleration
  Vec2  wind {0,0};           // wind effect (pixels/sec)
  float stoke = 0.0f;         // 0..1 boost from upward motion

  // Rotational motion
  Vec3  angularVel {0,0,0};   // angular velocity (rad/s)
  Vec3  smoothAngularVel {0,0,0}; // smoothed angular velocity
  float spinMagnitude = 0.0f; // overall rotation speed
  float centrifugalForce = 0.0f; // outward force from rotation

  // Advanced torch effects
  float flameDirection = 0.0f;  // angle (degrees) flames lean due to motion
  float flameBend = 0.0f;       // 0-1 how much flames bend from vertical
  float turbulenceLevel = 0.0f; // 0-1 motion-induced turbulence
  Vec2  inertiaDrift {0,0};     // momentum-based drift effects

  // Motion analysis
  float motionIntensity = 0.0f; // overall motion level (0-1)
  float jerkMagnitude = 0.0f;   // rate of acceleration change
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

    // Enhanced physics methods
    void updateTorchOrientation_(const Vec3& accel, const Vec3& gyro, float dt);
    void updateWindPhysics_(const Vec3& linAccel, const Vec3& angularVel, float dt);
    void updateRotationalEffects_(const Vec3& angularVel, float dt);
    void updateStokePhysics_(const Vec3& linAccel, const Vec3& velocity, float dt);
    void updateMotionAnalysis_(const Vec3& accel, const Vec3& gyro, float dt);
    void applyMotionSmoothing_(float dt);
    Vec3 calculateCentrifugalForce_(const Vec3& angularVel);
    Vec3 calculateCoriolisEffect_(const Vec3& velocity, const Vec3& angularVel);

    // state
    MotionConfig cfg_;
    MotionState  motion_;
    Vec3         gLP_{0,0,9.81f};  // low-pass gravity estimate

    // Enhanced physics tracking
    Vec3         prevAccel_{0,0,0};     // previous acceleration for jerk calculation
    Vec3         prevAngularVel_{0,0,0}; // previous angular velocity
    Vec3         rawVelocity_{0,0,0};   // integrated raw velocity
    Vec3         flameVelocity_{0,0,0}; // flame-specific velocity with inertia

    // Motion history for advanced analysis
    static const int MOTION_HISTORY_SIZE = 8;
    Vec3 accelHistory_[MOTION_HISTORY_SIZE];
    Vec3 gyroHistory_[MOTION_HISTORY_SIZE];
    int historyIndex_ = 0;

    // Timing for realistic physics
    uint32_t lastUpdateMs_ = 0;
    float accumDt_ = 0.0f;  // accumulated time for sub-frame calculations
};
