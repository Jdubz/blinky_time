#include "SerialConsole.h"
#include "TotemDefaults.h"

static String readLine() {
  String s;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') break;
    s += c;
  }
  return s;
}

void SerialConsole::begin(FireParams* params) {
  if (params) {
    // Bind to provided params reference
    memcpy(&p, params, sizeof(FireParams));
  }
  Serial.println(F("SerialConsole ready. Type 'show' or 'set <key> <value>'."));
}

void SerialConsole::tick() {
  if (!Serial.available()) return;
  String line = readLine();
  if (line.length() == 0) return;
  line.trim();
  if (line.equalsIgnoreCase("show") || line.equalsIgnoreCase("print")) {
    printAll();
    return;
  }
  if (line.startsWith("set ")) {
    line.remove(0, 4);
    int sp = line.indexOf(' ');
    if (sp > 0) {
      String key = line.substring(0, sp);
      String val = line.substring(sp + 1);
      float f = val.toFloat();
      if (handleSet(key.c_str(), f)) {
        Serial.println(F("ok"));
      } else {
        Serial.println(F("unknown key"));
      }
    }
    return;
  }
  if (line.equalsIgnoreCase("defaults")) {
    restoreDefaults();
    Serial.println(F("defaults restored"));
    return;
  }
  Serial.println(F("commands: show | set <key> <value> | defaults"));
}

void SerialConsole::restoreDefaults() {
  p.fluidEnabled     = Defaults::FluidEnabled;
  p.viscosity        = Defaults::Viscosity;
  p.heatDiffusion    = Defaults::HeatDiffusion;
  p.updraftBase      = Defaults::UpdraftBase;
  p.buoyancy         = Defaults::Buoyancy;
  p.swirlAmp         = Defaults::SwirlAmp;
  p.swirlScaleCells  = Defaults::SwirlScaleCells;
  p.swirlAudioGain   = Defaults::SwirlAudioGain;
  p.baseCooling      = Defaults::BaseCooling;
  p.coolingAudioBias = Defaults::CoolingAudioBias;
  p.sparkChance      = Defaults::SparkChance;
  p.sparkHeatMin     = Defaults::SparkHeatMin;
  p.sparkHeatMax     = Defaults::SparkHeatMax;
  p.audioHeatBoostMax= Defaults::AudioHeatBoostMax;
  p.audioSparkBoost  = Defaults::AudioSparkBoost;
  p.vuTopRowEnabled  = Defaults::VuTopRowEnabled;
  p.brightnessCap    = Defaults::BrightnessCap;
}

void SerialConsole::printAll() {
  Serial.println(F("--- Fire Params ---"));
  Serial.print(F("fluidEnabled: "));    Serial.println(p.fluidEnabled ? F("true") : F("false"));
  Serial.print(F("viscosity: "));       Serial.println(p.viscosity, 3);
  Serial.print(F("heatDiffusion: "));   Serial.println(p.heatDiffusion, 3);
  Serial.print(F("updraftBase: "));     Serial.println(p.updraftBase, 3);
  Serial.print(F("buoyancy: "));        Serial.println(p.buoyancy, 3);
  Serial.print(F("swirlAmp: "));        Serial.println(p.swirlAmp, 3);
  Serial.print(F("swirlScaleCells: ")); Serial.println(p.swirlScaleCells, 3);
  Serial.print(F("swirlAudioGain: "));  Serial.println(p.swirlAudioGain, 3);
  Serial.print(F("baseCooling: "));     Serial.println(p.baseCooling, 1);
  Serial.print(F("coolingAudioBias: "));Serial.println(p.coolingAudioBias, 1);
  Serial.print(F("sparkChance: "));     Serial.println(p.sparkChance, 3);
  Serial.print(F("sparkHeatMin: "));    Serial.println(p.sparkHeatMin, 1);
  Serial.print(F("sparkHeatMax: "));    Serial.println(p.sparkHeatMax, 1);
  Serial.print(F("audioHeatBoostMax: ")); Serial.println(p.audioHeatBoostMax, 1);
  Serial.print(F("audioSparkBoost: ")); Serial.println(p.audioSparkBoost, 3);
  Serial.print(F("vuTopRowEnabled: ")); Serial.println(p.vuTopRowEnabled ? F("on") : F("off"));
  Serial.print(F("brightnessCap: "));   Serial.println(p.brightnessCap, 2);
}

bool SerialConsole::handleSet(const char* key, float value) {
  if (!strcmp(key, "viscosity"))        { p.viscosity = value; }
  else if (!strcmp(key, "heatdiffusion")) { p.heatDiffusion = value; }
  else if (!strcmp(key, "updraft"))     { p.updraftBase = value; }
  else if (!strcmp(key, "buoyancy"))    { p.buoyancy = value; }
  else if (!strcmp(key, "swirlamp"))    { p.swirlAmp = value; }
  else if (!strcmp(key, "swirlscale"))  { p.swirlScaleCells = value; }
  else if (!strcmp(key, "swirlaudiogain")) { p.swirlAudioGain = value; }
  else if (!strcmp(key, "basecooling")) { p.baseCooling = value; }
  else if (!strcmp(key, "coolingaudiobias")) { p.coolingAudioBias = value; }
  else if (!strcmp(key, "sparkchance")) { p.sparkChance = value; }
  else if (!strcmp(key, "sparkheatmin")) { p.sparkHeatMin = value; }
  else if (!strcmp(key, "sparkheatmax")) { p.sparkHeatMax = value; }
  else if (!strcmp(key, "audioheatmax")) { p.audioHeatBoostMax = value; }
  else if (!strcmp(key, "audiosparkboost")) { p.audioSparkBoost = value; }
  else if (!strcmp(key, "brightnesscap")) {
    if (value < 0.05f) value = 0.05f;
    if (value > 1.00f) value = 1.00f;
    p.brightnessCap = value;
  }
  else {
    return false;
  }
  return true;
}
