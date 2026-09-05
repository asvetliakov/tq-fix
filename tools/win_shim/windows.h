// Minimal Win32 typedef shim so the DXBC transforms in src/dxbc_patch.cpp can
// be compiled and exercised on the host against the extracted shader corpus.
// It supplies only the scalar types those transforms name; anything that needs
// a real Win32 API belongs in the in-bottle self-test instead.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef unsigned char BYTE;
typedef size_t SIZE_T;
typedef unsigned int UINT;
