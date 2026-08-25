// The winmm proxy: 186 exported stubs, each a one-instruction jump through a
// slot that this fills at attach from the real winmm.
//
// Adapted from ../grimdawn-trash/src/winmm_proxy.{h,cpp}. The design decision it
// records — resolve the real winmm ourselves rather than `.def`-forward to a
// second file the installer copied — is theirs, and the reason is distribution:
// the second file can never go in a ZIP. Do not re-derive it.
//
// What is different here is the architecture. See the hazard note in the .cpp.

#pragma once

#include <windows.h>

namespace tq {
namespace winmm {

/**
 * Load the real winmm and fill all 186 slots from it.
 *
 * Called from `DLL_PROCESS_ATTACH`, and it is the one thing this DLL must get
 * right before anything else happens: until it returns, every winmm entry point
 * in the process points at a stub that does nothing.
 */
void resolve(HINSTANCE self);

/** One line saying how it went. `when` is "attach" or "detach". */
void report(const char* when);

}  // namespace winmm
}  // namespace tq
