// crashlog.h -- last-resort diagnostics.
//
// Installs an unhandled-exception filter that writes bin/crash.log with the
// exception code, faulting address, and the module it landed in. Combined
// with the mirrored bin/loopcore.log, that is usually enough to place a
// crash without a debugger attached.
#pragma once

#include <string>

namespace lc {

// `dir` is where crash.log is written. Safe to call once, early.
void InstallCrashHandler(const std::string& dir);

} // namespace lc
