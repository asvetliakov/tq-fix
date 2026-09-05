#pragma once
#include <windows.h>
#include "probe.h"

namespace tq { namespace secondaryadmission {

// Both renderer draw sites must be hooked before the shared per-frame budget
// can omit a renderable. Its frame serial is independent of the recorder.
bool secondaryPassAdmissionRequested();
void setSecondaryAdmissionDrawHooksReady(bool ready);
void secondaryAdmissionFrameBoundary();
namespace detail {
extern volatile LONG secondaryAdmissionDrawSuppressDepth;
}
inline bool secondaryAdmissionDrawSuppressed() {
    return detail::secondaryAdmissionDrawSuppressDepth != 0;
}
inline void noteSecondaryAdmissionDrawSkipped() {
    tq::probe::engineCount(tq::probe::CounterEngineSecondaryAdmissionDrawSkipped);
}

} }
