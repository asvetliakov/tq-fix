#pragma once
#include <windows.h>
namespace tq { namespace meshpreload {
void readOptions(const wchar_t* path);
bool configured();
bool install(HMODULE engine);
void shutdown();
void report();
#ifdef TQ_SELFTEST
bool test();
#endif
} }
