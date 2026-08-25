// The log file is the debugger.
//
// There is no usable debugger inside the bottle under FEX (CLAUDE.md), so every
// fact this project learns about the running game will arrive through this file.
// Adapted from ../grimdawn-trash/src/log.{h,cpp}; the two-level split is its
// design and the reasoning below is its reasoning.
//
// Every line is flushed, because the interesting run is the one that ends in a
// crash. Nothing here allocates and nothing here can throw.

#pragma once

#include <windows.h>
#include <stdio.h>  // for __MINGW_PRINTF_FORMAT, which the attribute below needs

namespace tq {

/** Open the log. Tries $TQFLICKER_LOG, then %TEMP%, then next to the DLL. */
void logOpen(HINSTANCE self);
void logClose();

/** Where it ended up, for the one line that says so. */
const wchar_t* logPath();

// `~/Documents` is unreadable from a macOS terminal under TCC — this project hit
// that independently in Stage 0, when the game's own options.txt could not be
// read. So the log goes to %TEMP% and there is no second copy under Documents.
// Do not "improve" this by writing one; nothing here could read it back.

void tqlog(const char* fmt, ...) __attribute__((format(__MINGW_PRINTF_FORMAT, 1, 2)));

/**
 * **A line that is only written when detail was asked for.**
 *
 * The rule for choosing, and it is not "how interesting is this line":
 *
 *   * Did something *happen*, once? — `tqlog`.
 *   * Is anything wrong or surprising? — `tqlog`, with `!!`.
 *   * Is this one row of a table, one item of many, or one frame of a loop? —
 *     `tqtrace`, and make sure a *count* of them survives in the report.
 *
 * This matters more here than it did next door. Stage 4 will log per-frame draw
 * counts at 10fps; that is a table with tens of thousands of rows, and it must
 * not be in the file somebody sends when the game misbehaves.
 */
void tqtrace(const char* fmt, ...) __attribute__((format(__MINGW_PRINTF_FORMAT, 1, 2)));

/** Turn `tqtrace` on. */
void logSetVerbose(bool on);
bool logIsVerbose();

/** A blank line. Spelled out because an empty format string is a warning. */
inline void tqblank() { tqlog("%s", ""); }

}  // namespace tq

using tq::tqblank;
using tq::tqlog;
using tq::tqtrace;
