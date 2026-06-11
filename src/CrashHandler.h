#pragma once

// Process-wide last-resort crash reporter. Installs a SetUnhandledExceptionFilter
// that, on any unhandled SEH exception (access violation, etc.), appends a
// human-readable report — exception code, faulting module+offset, and a
// symbolized stack trace — to "winzoo-crash.txt" next to the executable, then
// lets the process terminate. Call once, as early as possible in WinMain.
namespace CrashHandler {
void Install();
}
