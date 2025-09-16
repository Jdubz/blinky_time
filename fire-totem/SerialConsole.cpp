#include "SerialConsole.h"
#include "TotemDefaults.h"
#include <ctype.h>
#include <string.h>

SerialConsole::SerialConsole(FireEffect* fire, AudioParams* audio, uint8_t maxRows, AdaptiveMic* mic)
: fire(fire), audio(audio), mic(mic), maxRows(maxRows) {}

void SerialConsole::begin() {
  restoreDefaults();
  Serial.println(F("fire-totem console ready. Type 'help'."));
  printAll();
}

void SerialConsole::restoreDefaults() {
  // Audio defaults
  audio->noiseGate   = Defaults::NoiseGate;
  audio->gamma       = Defaults::Gamma;
  audio->globalGain  = Defaults::GlobalGain;
  audio->attackTau   = Defaults::AttackTau;
  audio->releaseTau  = Defaults::ReleaseTau;

  // Fire defaults
  FireParams p;
  p.baseCooling         = Defaults::BaseCooling;
  p.sparkHeatMin        = Defaults::SparkHeatMin;
  p.sparkHeatMax        = Defaults::SparkHeatMax;
  p.sparkChance         = Defaults::SparkChance;
  p.audioSparkBoost     = Defaults::AudioSparkBoost;
  p.audioHeatBoostMax   = Defaults::AudioHeatBoostMax;
  p.coolingAudioBias    = Defaults::CoolingAudioBias;
  p.bottomRowsForSparks = Defaults::BottomRowsForSparks;

  p.fluidEnabled    = Defaults::FluidEnabled;
  p.buoyancy        = Defaults::Buoyancy;
  p.viscosity       = Defaults::Viscosity;
  p.heatDiffusion   = Defaults::HeatDiffusion;
  p.swirlAmp        = Defaults::SwirlAmp;
  p.swirlAudioGain  = Defaults::SwirlAudioGain;
  p.swirlScaleCells = Defaults::SwirlScaleCells;
  p.updraftBase     = Defaults::UpdraftBase;
  p.vuTopRowEnabled = Defaults::VuTopRowEnabled;
  
  fire->setParams(p);

  // Bass filter defaults
  if (mic) mic->setBassFilter(Defaults::BassEnabledDefault,
                               Defaults::BassFc,
                               Defaults::BassQ,
                               Defaults::BassModeDefault);
}

void SerialConsole::printHelp() {
  Serial.println(F("\nCommands:"));
  Serial.println(F("  show                         -> print current values"));
  Serial.println(F("  defaults                     -> restore defaults"));
  Serial.println(F("  help                         -> this help\n"));
  Serial.println(F("  set gate <f>                 -> noise gate (0..0.5)"));
  Serial.println(F("  set gamma <f>                -> gamma (0.2..2.5)"));
  Serial.println(F("  set gain <f>                 -> global gain (0..5)"));
  Serial.println(F("  set attack <f>               -> attack tau sec (0.005..1)"));
  Serial.println(F("  set release <f>              -> release tau sec (0.02..2)"));
  Serial.println(F("  set cooling <i>              -> base cooling (0..255)"));
  Serial.println(F("  set sparkmin <i>             -> spark heat min (0..255)"));
  Serial.println(F("  set sparkmax <i>             -> spark heat max (0..255)"));
  Serial.println(F("  set sparkchance <f>          -> base spark chance (0..1)"));
  Serial.println(F("  set sparkboost <f>           -> audio spark boost (0..2)"));
  Serial.println(F("  set heatboost <i>            -> audio heat boost max (0..255)"));
  Serial.println(F("  set coolbias <i>             -> cooling audio bias (-127..127)"));
  Serial.println(F("  set sparkrows <i>            -> bottom rows for sparks (1..max)"));
  Serial.println(F("  set vu on|off                -> show/hide top-row VU meter"));
  Serial.println(F("  set bass on|off              -> enable/disable bass filter"));
  Serial.println(F("  set bassfreq <Hz>            -> center/cutoff (30..400)"));
  Serial.println(F("  set bassq <Q>                -> Q factor (0.3..5.0)"));
  Serial.println(F("  set basstype lp|bp           -> low-pass or band-pass"));
  Serial.println(F("  set fluid on|off             -> enable/disable fluid advection"));
  Serial.println(F("  set buoy <f>                 -> buoyancy (cells/s^2 per heat)"));
  Serial.println(F("  set visc <f>                 -> velocity viscosity 0..1"));
  Serial.println(F("  set heatdiff <f>             -> heat diffusion 0..1"));
  Serial.println(F("  set swirlamp <f>             -> swirl speed (cells/s)"));
  Serial.println(F("  set swirlaudio <f>           -> extra swirl per energy"));
  Serial.println(F("  set swirlscale <f>           -> swirl spatial period (cells)"));
  Serial.println(F("  set updraft <f>              -> constant upward accel (cells/s^2)"));
}

void SerialConsole::printAll() {
  FireParams p = fire->getParams();
  bool bOn; float bFc, bQ; AdaptiveMic::BassMode bMode;
  if (mic) mic->getBassFilter(bOn, bFc, bQ, bMode); else { bOn=false; bFc=0; bQ=0; bMode=AdaptiveMic::BASS_BANDPASS; }

  Serial.print(F("[AUDIO] gate="));   Serial.print(audio->noiseGate, 3);
  Serial.print(F(" gamma="));         Serial.print(audio->gamma, 3);
  Serial.print(F(" gain="));          Serial.print(audio->globalGain, 3);
  Serial.print(F(" attack="));        Serial.print(audio->attackTau, 3);
  Serial.print(F(" release="));       Serial.print(audio->releaseTau, 3);
  Serial.println();

  Serial.print(F("[VU] topRow="));
  Serial.println(p.vuTopRowEnabled ? F("on") : F("off"));
  
  Serial.print(F("[BASS ] enabled=")); Serial.print(bOn ? F("on") : F("off"));
  Serial.print(F(" mode="));          Serial.print(bMode == AdaptiveMic::BASS_LOWPASS ? F("lp") : F("bp"));
  Serial.print(F(" fc="));            Serial.print(bFc, 1);
  Serial.print(F(" Q="));             Serial.print(bQ, 2);
  Serial.print(F(" fs="));            Serial.print(mic ? mic->getSampleRate() : 0, 0);
  Serial.println();

  Serial.print(F("[FIRE ] cooling="));    Serial.print(p.baseCooling);
  Serial.print(F(" spark(min..max)="));   Serial.print(p.sparkHeatMin); Serial.print(".."); Serial.print(p.sparkHeatMax);
  Serial.print(F(" chance="));            Serial.print(p.sparkChance, 3);
  Serial.print(F(" sparkBoost="));        Serial.print(p.audioSparkBoost, 3);
  Serial.print(F(" heatBoostMax="));      Serial.print(p.audioHeatBoostMax);
  Serial.print(F(" coolBias="));          Serial.print(p.coolingAudioBias);
  Serial.print(F(" rows="));              Serial.print(p.bottomRowsForSparks);
  Serial.println();

  Serial.print(F("[FLUID] on=")); Serial.print(p.fluidEnabled ? F("on") : F("off"));
  Serial.print(F(" buoy="));      Serial.print(p.buoyancy, 2);
  Serial.print(F(" visc="));      Serial.print(p.viscosity, 2);
  Serial.print(F(" heatDiff="));  Serial.print(p.heatDiffusion, 2);
  Serial.print(F(" swirlAmp="));  Serial.print(p.swirlAmp, 2);
  Serial.print(F(" swirlAud="));  Serial.print(p.swirlAudioGain, 2);
  Serial.print(F(" scale="));     Serial.print(p.swirlScaleCells, 2);
  Serial.print(F(" updraft="));   Serial.print(p.updraftBase, 2);
  Serial.println();
}

bool SerialConsole::readLine() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      buf[min<int>(len, kBufSize-1)] = '\0';
      len = 0;
      return true;
    }
    if (len < kBufSize-1) buf[len++] = c;
  }
  return false;
}

void SerialConsole::toLowerInPlace(char* s) { for (; *s; ++s) *s = (char)tolower(*s); }

void SerialConsole::handleCommand(const char* line) {
  char tmp[kBufSize];
  strncpy(tmp, line, sizeof(tmp));
  tmp[sizeof(tmp)-1] = '\0';
  toLowerInPlace(tmp);

  char* save;
  char* tok = strtok_r(tmp, " ", &save);
  if (!tok) return;

  if (!strcmp(tok, "show") || !strcmp(tok, "print") || !strcmp(tok, "?")) { printAll(); return; }
  if (!strcmp(tok, "help") || !strcmp(tok, "h")) { printHelp(); return; }
  if (!strcmp(tok, "defaults")) { restoreDefaults(); printAll(); return; }

  if (!strcmp(tok, "set")) {
    char* key = strtok_r(nullptr, " ", &save);
    char* val = strtok_r(nullptr, " ", &save);
    if (!key) { Serial.println(F("ERR: set <key> <value>")); return; }

    FireParams p = fire->getParams();

    // ---- Audio keys ----
    if (!strcmp(key, "gate"))        { if (!val) {Serial.println(F("ERR")); return;} audio->noiseGate  = constrain((float)atof(val), Defaults::Ranges::NoiseGateMin,  Defaults::Ranges::NoiseGateMax); }
    else if (!strcmp(key, "gamma"))  { if (!val) {Serial.println(F("ERR")); return;} audio->gamma      = constrain((float)atof(val), Defaults::Ranges::GammaMin,      Defaults::Ranges::GammaMax); }
    else if (!strcmp(key, "gain"))   { if (!val) {Serial.println(F("ERR")); return;} audio->globalGain = constrain((float)atof(val), Defaults::Ranges::GainMin,       Defaults::Ranges::GainMax); }
    else if (!strcmp(key, "attack")) { if (!val) {Serial.println(F("ERR")); return;} audio->attackTau  = constrain((float)atof(val), Defaults::Ranges::AttackMin,     Defaults::Ranges::AttackMax); }
    else if (!strcmp(key, "release")){ if (!val) {Serial.println(F("ERR")); return;} audio->releaseTau = constrain((float)atof(val), Defaults::Ranges::ReleaseMin,    Defaults::Ranges::ReleaseMax); }
    else if (!strcmp(key, "vu")) {
      if (!val) { Serial.println(F("ERR")); return; }
      if      (!strcmp(val, "on"))  p.vuTopRowEnabled = true;
      else if (!strcmp(val, "off")) p.vuTopRowEnabled = false;
      else { Serial.println(F("ERR: vu on|off")); return; }
    }



    // ---- Fire keys ----
    else if (!strcmp(key, "cooling"))     { if (!val) {Serial.println(F("ERR")); return;} p.baseCooling         = (uint8_t)constrain(atoi(val), Defaults::Ranges::CoolingMin, Defaults::Ranges::CoolingMax); }
    else if (!strcmp(key, "sparkmin"))    { if (!val) {Serial.println(F("ERR")); return;} p.sparkHeatMin        = (uint8_t)constrain(atoi(val), 0, 255); }
    else if (!strcmp(key, "sparkmax"))    { if (!val) {Serial.println(F("ERR")); return;} p.sparkHeatMax        = (uint8_t)constrain(atoi(val), 0, 255); }
    else if (!strcmp(key, "sparkchance")) { if (!val) {Serial.println(F("ERR")); return;} p.sparkChance         = constrain((float)atof(val), Defaults::Ranges::SparkChanceMin, Defaults::Ranges::SparkChanceMax); }
    else if (!strcmp(key, "sparkboost"))  { if (!val) {Serial.println(F("ERR")); return;} p.audioSparkBoost     = constrain((float)atof(val), 0.0f, 2.0f); }
    else if (!strcmp(key, "heatboost"))   { if (!val) {Serial.println(F("ERR")); return;} p.audioHeatBoostMax   = (uint8_t)constrain(atoi(val), 0, 255); }
    else if (!strcmp(key, "coolbias"))    { if (!val) {Serial.println(F("ERR")); return;} p.coolingAudioBias    = (int8_t) constrain(atoi(val), -127, 127); }
    else if (!strcmp(key, "sparkrows"))   { if (!val) {Serial.println(F("ERR")); return;} p.bottomRowsForSparks = (uint8_t)constrain(atoi(val), 1, (int)maxRows); }

    // ---- Fluid / swirl keys ----
    else if (!strcmp(key, "fluid"))     { if (!val) {Serial.println(F("ERR")); return;} 
    if (!strcmp(val,"on"))  p.fluidEnabled = true;
    else if (!strcmp(val,"off")) p.fluidEnabled = false;
    else { Serial.println(F("ERR: fluid on|off")); return; } }
    else if (!strcmp(key, "buoy"))      { if (!val) {Serial.println(F("ERR")); return;} p.buoyancy       = (float)atof(val); }
    else if (!strcmp(key, "visc"))      { if (!val) {Serial.println(F("ERR")); return;} p.viscosity      = constrain((float)atof(val), 0.0f, 1.0f); }
    else if (!strcmp(key, "heatdiff"))  { if (!val) {Serial.println(F("ERR")); return;} p.heatDiffusion  = constrain((float)atof(val), 0.0f, 1.0f); }
    else if (!strcmp(key, "swirlamp"))  { if (!val) {Serial.println(F("ERR")); return;} p.swirlAmp       = (float)atof(val); }
    else if (!strcmp(key, "swirlaudio")){ if (!val) {Serial.println(F("ERR")); return;} p.swirlAudioGain = (float)atof(val); }
    else if (!strcmp(key, "swirlscale")){ if (!val) {Serial.println(F("ERR")); return;} p.swirlScaleCells= max(1.0f, (float)atof(val)); }
    else if (!strcmp(key, "updraft")) {
      if (!val) { Serial.println(F("ERR")); return; }
      p.updraftBase = (float)atof(val);
    }

    // ---- Bass keys ----
    else if (!strcmp(key, "bass")) {
      if (!val) { Serial.println(F("ERR: set bass on|off")); return; }
      if (!mic) { Serial.println(F("ERR: mic null")); return; }
      if (!strcmp(val, "on"))       mic->setBassFilter(true);
      else if (!strcmp(val, "off")) mic->setBassFilter(false);
      else { Serial.println(F("ERR: set bass on|off")); return; }
    }
    else if (!strcmp(key, "bassfreq")) {
      if (!val || !mic) { Serial.println(F("ERR")); return; }
      float hz = constrain((float)atof(val), Defaults::Ranges::BassFreqMin, Defaults::Ranges::BassFreqMax);
      bool bOn; float bFc, bQ; AdaptiveMic::BassMode bMode;
      mic->getBassFilter(bOn, bFc, bQ, bMode);
      mic->setBassFilter(bOn, hz, bQ, bMode);
    }
    else if (!strcmp(key, "bassq")) {
      if (!val || !mic) { Serial.println(F("ERR")); return; }
      float q = constrain((float)atof(val), Defaults::Ranges::BassQMin, Defaults::Ranges::BassQMax);
      bool bOn; float bFc, bQ; AdaptiveMic::BassMode bMode;
      mic->getBassFilter(bOn, bFc, bQ, bMode);
      mic->setBassFilter(bOn, bFc, q, bMode);
    }
    else if (!strcmp(key, "basstype")) {
      if (!val || !mic) { Serial.println(F("ERR")); return; }
      AdaptiveMic::BassMode m;
      if (!strcmp(val, "lp")) m = AdaptiveMic::BASS_LOWPASS;
      else if (!strcmp(val, "bp")) m = AdaptiveMic::BASS_BANDPASS;
      else { Serial.println(F("ERR: basstype lp|bp")); return; }
      bool bOn; float bFc, bQ; AdaptiveMic::BassMode bMode;
      mic->getBassFilter(bOn, bFc, bQ, bMode);
      mic->setBassFilter(bOn, bFc, bQ, m);
    }

    else { Serial.println(F("ERR: unknown key (type 'help')")); return; }

    fire->setParams(p);
    Serial.print(F("OK "));
    printAll();
    return;
  }

  Serial.println(F("ERR: unknown cmd (type 'help')"));
}

void SerialConsole::update() {
  if (readLine()) handleCommand(buf);

  // periodic print every 5s
  unsigned long now = millis();
  if (now - lastStatusMs >= 5000UL) {
    printAll();
    lastStatusMs = now;
  }
}
