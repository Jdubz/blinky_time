// Vendored Monocypher (ota/monocypher/README.md), compiled as its own C
// translation unit. It must NOT go through BlinkyImplementations.h: that
// puts every implementation in one unit, and Monocypher's internal macros
// (FOR, COPY, MIN, ...) would leak into all the code after it. Root-level
// sources are compiled separately by the Arduino build (cf. Nrf52PdmMic.cpp).
#include "ota/monocypher/monocypher.c"
