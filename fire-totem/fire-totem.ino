#include <Adafruit_NeoPixel.h>
#include "FireEffect.h"
#include "TotemDefaults.h"
#include "AdaptiveMic.h"
#include "IMUHelper.h"
#include "SerialConsole.h"
constexpr uint8_t LED_PIN = D10;
Adafruit_NeoPixel strip(Defaults::Width * Defaults::Height, LED_PIN, NEO_GRB + NEO_KHZ800);
AdaptiveMic   mic; IMUHelper imu; FireParams fp; FireEffect* fire=nullptr; SerialConsole* consolePtr=nullptr;
//#define MIC_HAS_HW_GAIN_SET_DB
//#define MIC_HAS_HW_GAIN_SET_LINEAR
static inline void micSetHardwareGain(AdaptiveMic& m, float linear) {
#if defined(MIC_HAS_HW_GAIN_SET_DB)
  float db = 20.0f * log10f(linear <= 1e-6f ? 1e-6f : linear);
  m.setGainDb(db);
#elif defined(MIC_HAS_HW_GAIN_SET_LINEAR)
  m.setHardwareGainLinear(linear);
#else
  (void)m; (void)linear;
#endif
}
struct RoomAGC {
  static const uint16_t SEC_BUF = 300;
  float secMax[SEC_BUF]; uint16_t secWrite=0; bool bufFilled=false; float curSecMax=0.0f; uint32_t lastSec=0;
  float dynGain=1.0f; const float minGain=0.2f, maxGain=10.0f;
  const uint16_t evalMs=250; uint32_t lastEval=0; uint16_t evalFrames=0, evalSatFrames=0; float evalMaxNorm=0.0f;
  float lastWinMaxNorm=0.0f, lastWinSatRatio=0.0f;
  float smallStep=0.01f, extremeFactor=5.0f, targetPeak=0.90f;
  void begin(){ for(uint16_t i=0;i<SEC_BUF;i++) secMax[i]=0; lastSec=millis(); lastEval=millis(); }
  float process(AdaptiveMic& hw, float raw){
    float adjusted = raw * dynGain; if(adjusted>1) adjusted=1; if(adjusted<0) adjusted=0;
    if(adjusted>curSecMax) curSecMax=adjusted; uint32_t now=millis();
    if(now-lastSec>=1000){ secMax[secWrite]=curSecMax; secWrite=(secWrite+1)%SEC_BUF; if(secWrite==0) bufFilled=true; curSecMax=0; lastSec=now; }
    float max5m=0; uint16_t count=bufFilled?SEC_BUF:secWrite; for(uint16_t i=0;i<count;i++) if(secMax[i]>max5m) max5m=secMax[i]; if(max5m<1e-6f) max5m=1e-6f;
    float norm=adjusted/max5m; if(norm>1) norm=1;
    evalFrames++; if(norm>=0.98f) evalSatFrames++; if(norm>evalMaxNorm) evalMaxNorm=norm;
    if(now-lastEval>=evalMs){ lastWinMaxNorm=evalMaxNorm; lastWinSatRatio=evalFrames?(float)evalSatFrames/(float)evalFrames:0; evalFrames=evalSatFrames=0; evalMaxNorm=0; lastEval=now; }
    float step=smallStep; bool extremeQuiet=(lastWinMaxNorm<0.50f); bool extremeLoud=(lastWinSatRatio>0.20f);
    if(extremeQuiet){ step*=extremeFactor; dynGain*=(1.0f+step); }
    else if(extremeLoud){ step*=extremeFactor; dynGain/=(1.0f+step); }
    else { float err=targetPeak-lastWinMaxNorm; dynGain*=(1.0f+step*err); }
    if(dynGain<minGain) dynGain=minGain; if(dynGain>maxGain) dynGain=maxGain; micSetHardwareGain(hw,dynGain);
    return norm;
  }
}; RoomAGC g_agc;
void setup(){
  Serial.begin(115200); strip.begin(); strip.setBrightness(Defaults::StripBrightness); strip.show();
  fp.width=Defaults::Width; fp.height=Defaults::Height; fp.fluidEnabled=Defaults::FluidEnabled; fp.viscosity=Defaults::Viscosity; fp.heatDiffusion=Defaults::HeatDiffusion;
  fp.updraftBase=Defaults::UpdraftBase; fp.buoyancy=Defaults::Buoyancy; fp.swirlAmp=Defaults::SwirlAmp; fp.swirlScaleCells=Defaults::SwirlScaleCells; fp.swirlAudioGain=Defaults::SwirlAudioGain;
  fp.baseCooling=Defaults::BaseCooling; fp.coolingAudioBias=Defaults::CoolingAudioBias; fp.sparkChance=Defaults::SparkChance; fp.sparkHeatMin=Defaults::SparkHeatMin; fp.sparkHeatMax=Defaults::SparkHeatMax;
  fp.audioHeatBoostMax=Defaults::AudioHeatBoostMax; fp.audioSparkBoost=Defaults::AudioSparkBoost; fp.bottomRowsForSparks=Defaults::BottomRowsForSparks;
  fp.vuTopRowEnabled=Defaults::VuTopRowEnabled; fp.brightnessCap=Defaults::BrightnessCap; fp.radiativeCooling=Defaults::RadiativeCooling; fp.topCoolingBoost=Defaults::TopCoolingBoost; fp.velDamping=Defaults::VelocityDamping;
  fire = new FireEffect(&strip, fp);
  mic.begin(); imu.begin(); g_agc.begin();
  consolePtr = new SerialConsole(fire, (uint8_t)fp.height, &mic); consolePtr->begin();
}
void loop(){
  float raw = mic.getLevel();
  float energy = g_agc.process(mic, raw);
  float ax=0, ay=0, az=0; if(!imu.getAccel(ax,ay,az)){ ax=ay=az=0; }
  fire->update(energy, ax, ay);
  fire->render();
  delay(16);
}
