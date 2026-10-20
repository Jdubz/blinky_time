#pragma once
// Test-only CPU cost hook for the host tracker harness (tests/tracker/).
//
// On the device a spectral frame and an NN inference take real time, and the
// PDM ISR keeps filling the mic ring meanwhile. The host harness has a
// virtual clock that only moves when the harness moves it. Without this hook
// it cannot reproduce timing bugs such as the b208-b210 drain-loop livelock
// (NN inference ~ one frame period, so update() never caught up). With
// BLINKY_HOST_HARNESS the tracker reports each unit of work and the harness
// advances its clock (delivering mic blocks) by the measured device cost.
// Device builds compile the macro to nothing.
#ifdef BLINKY_HOST_HARNESS
namespace HostCost {
enum Work { FRAME = 0, NN_INFER = 1 };
extern void (*onWork)(int work);  // defined by the harness; nullptr = free
}
#define BLINKY_HOST_COST(work) do { if (HostCost::onWork) HostCost::onWork(work); } while (0)
#else
#define BLINKY_HOST_COST(work) do {} while (0)
#endif
