#include "CrashHandler.h"

#include <windows.h>
#include <dbghelp.h>
#include <strsafe.h>

#include "winzoo_version.h"

#pragma comment(lib, "dbghelp.lib")

namespace {

// Re-entrancy / one-shot guard: if a second thread faults (or the handler itself
// faults) while we're writing, don't recurse — just let it terminate.
LONG g_inHandler = 0;

// --- Tiny append-to-fixed-buffer formatter (no heap, CRT-lock-free) ----------
struct Buf {
    char*  p;       // write cursor
    size_t left;    // bytes remaining (keeps room for the NUL StringCch* writes)
};

void Appendf(Buf& b, _Printf_format_string_ const char* fmt, ...)
{
    if (b.left <= 1) return;
    va_list ap;
    va_start(ap, fmt);
    char* end = nullptr;
    size_t rem = 0;
    if (SUCCEEDED(StringCchVPrintfExA(b.p, b.left, &end, &rem, 0, fmt, ap))) {
        b.left -= static_cast<size_t>(end - b.p);
        b.p = end;
    }
    va_end(ap);
}

// Module file name (no path) and offset of `addr` within its module. Works
// without any symbols — VirtualQuery gives the module's load base. Returns
// "?" if the address belongs to no mapped module.
void ModuleAndOffset(void* addr, char* nameOut, size_t nameCch, DWORD64& offsetOut)
{
    StringCchCopyA(nameOut, nameCch, "?");
    offsetOut = reinterpret_cast<DWORD64>(addr);

    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(addr, &mbi, sizeof(mbi)) || !mbi.AllocationBase) return;

    HMODULE hMod = reinterpret_cast<HMODULE>(mbi.AllocationBase);
    wchar_t pathW[MAX_PATH];
    DWORD n = GetModuleFileNameW(hMod, pathW, MAX_PATH);
    if (n == 0) return;

    const wchar_t* base = pathW;
    for (const wchar_t* s = pathW; *s; ++s)
        if (*s == L'\\' || *s == L'/') base = s + 1;

    WideCharToMultiByte(CP_UTF8, 0, base, -1, nameOut, static_cast<int>(nameCch), nullptr, nullptr);
    offsetOut = reinterpret_cast<DWORD64>(addr) - reinterpret_cast<DWORD64>(hMod);
}

const char* ExceptionName(DWORD code)
{
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case 0xE06D7363:                      return "C++ exception (unhandled)";
        default:                              return "unknown";
    }
}

// Append a symbolized call stack to the buffer. Best-effort: if DbgHelp can't
// load symbols (no .pdb), frames still show module+offset, which a .pdb/.map
// resolves after the fact.
void WriteStack(Buf& b, CONTEXT* ctx)
{
    HANDLE proc = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    BOOL haveSym = SymInitialize(proc, nullptr, TRUE);

    CONTEXT local = *ctx;   // StackWalk64 mutates the context — never the original.

    STACKFRAME64 frame{};
    DWORD machine;
#if defined(_M_X64)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset    = local.Rip;
    frame.AddrFrame.Offset = local.Rbp;
    frame.AddrStack.Offset = local.Rsp;
#elif defined(_M_ARM64)
    machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset    = local.Pc;
    frame.AddrFrame.Offset = local.Fp;
    frame.AddrStack.Offset = local.Sp;
#else
    machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset    = local.Eip;
    frame.AddrFrame.Offset = local.Ebp;
    frame.AddrStack.Offset = local.Esp;
#endif
    frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    Appendf(b, "\r\nCall stack:\r\n");

    // Storage for the resolved symbol name (lives across the loop).
    char symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];

    for (int i = 0; i < 64; ++i) {
        if (!StackWalk64(machine, proc, thread, &frame, &local, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        DWORD64 pc = frame.AddrPC.Offset;
        if (pc == 0) break;

        char   modName[MAX_PATH];
        DWORD64 modOff = 0;
        ModuleAndOffset(reinterpret_cast<void*>(pc), modName, sizeof(modName), modOff);

        // Symbol name + displacement.
        const char* symName = nullptr;
        DWORD64 symDisp = 0;
        if (haveSym) {
            auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
            ZeroMemory(sym, sizeof(SYMBOL_INFO));
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen   = MAX_SYM_NAME;
            if (SymFromAddr(proc, pc, &symDisp, sym))
                symName = sym->Name;
        }

        // Source file:line, if line info is present.
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        BOOL haveLine = haveSym && SymGetLineFromAddr64(proc, pc, &lineDisp, &line);

        if (symName && haveLine) {
            const char* file = line.FileName;
            for (const char* s = line.FileName; *s; ++s)
                if (*s == '\\' || *s == '/') file = s + 1;
            Appendf(b, "  [%02d] %s!%s+0x%llx  (%s:%lu)\r\n",
                    i, modName, symName, symDisp, file, line.LineNumber);
        } else if (symName) {
            Appendf(b, "  [%02d] %s!%s+0x%llx\r\n", i, modName, symName, symDisp);
        } else {
            Appendf(b, "  [%02d] %s+0x%llx\r\n", i, modName, modOff);
        }
    }

    if (haveSym) SymCleanup(proc);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    // First fault wins; anything that faults afterwards bails to the default
    // handler (process terminates) rather than re-entering DbgHelp.
    if (InterlockedCompareExchange(&g_inHandler, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    char buf[32 * 1024];
    Buf b{ buf, sizeof(buf) };

    SYSTEMTIME st;
    GetLocalTime(&st);

    const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
    DWORD code = er ? er->ExceptionCode : 0;

    Appendf(b, "================ winzoo crash ================\r\n");
    Appendf(b, "Time      : %04u-%02u-%02u %02u:%02u:%02u\r\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    Appendf(b, "Version   : %s\r\n", WINZOO_VERSION_STRING_FULL);

    wchar_t exePathW[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exePathW, MAX_PATH);
    char exePathU[MAX_PATH * 2] = "";
    WideCharToMultiByte(CP_UTF8, 0, exePathW, -1, exePathU, sizeof(exePathU), nullptr, nullptr);
    Appendf(b, "Process   : %s (PID %lu)\r\n", exePathU, GetCurrentProcessId());
    Appendf(b, "Thread    : %lu\r\n", GetCurrentThreadId());

    Appendf(b, "Exception : 0x%08lX (%s)", code, ExceptionName(code));
    if (er && code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const char* op = er->ExceptionInformation[0] == 1 ? "writing"
                       : er->ExceptionInformation[0] == 8 ? "executing"
                                                          : "reading";
        Appendf(b, " %s 0x%016llx", op,
                static_cast<DWORD64>(er->ExceptionInformation[1]));
    }
    Appendf(b, "\r\n");

    if (er && er->ExceptionAddress) {
        char   modName[MAX_PATH];
        DWORD64 modOff = 0;
        ModuleAndOffset(er->ExceptionAddress, modName, sizeof(modName), modOff);
        Appendf(b, "Fault IP  : 0x%016llx  %s+0x%llx\r\n",
                reinterpret_cast<DWORD64>(er->ExceptionAddress), modName, modOff);
    }

    if (ep && ep->ContextRecord)
        WriteStack(b, ep->ContextRecord);

    Appendf(b, "==============================================\r\n\r\n");

    // Write next to the executable: <exe dir>\winzoo-crash.txt, appended so a
    // run's history isn't clobbered by the next crash.
    wchar_t logPath[MAX_PATH];
    StringCchCopyW(logPath, MAX_PATH, exePathW);
    wchar_t* slash = nullptr;
    for (wchar_t* s = logPath; *s; ++s)
        if (*s == L'\\' || *s == L'/') slash = s;
    if (slash) slash[1] = L'\0'; else logPath[0] = L'\0';
    StringCchCatW(logPath, MAX_PATH, L"winzoo-crash.txt");

    HANDLE h = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, buf, static_cast<DWORD>(b.p - buf), &written, nullptr);
        FlushFileBuffers(h);
        CloseHandle(h);
    }

    // Terminate via the report we just wrote; don't pop the OS crash UI.
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

namespace CrashHandler {

void Install()
{
    SetUnhandledExceptionFilter(CrashFilter);
}

} // namespace CrashHandler
