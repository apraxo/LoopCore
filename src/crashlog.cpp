// crashlog.cpp
#include "crashlog.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <ctime>

namespace lc {

namespace {

// A fixed buffer, not a std::string.
//
// The path is built inside the exception filter, and building it with a
// std::string allocates -- at the one moment allocation is least safe. Heap
// corruption is a common reason to be in this handler at all, and a crash
// while writing the crash report loses the report and the reason for it.
//
// The path is known at startup, so it is formatted once there and the filter
// only reads it.
char g_dir[MAX_PATH * 2] = {};
char g_logPath[MAX_PATH * 2] = {};

const char* ExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
    case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE";
    case 0xE06D7363:                      return "C++ exception (unhandled)";
    default:                              return "unknown";
    }
}

// Which loaded module contains this address? Says whether the fault is ours,
// TensorRT's, or a driver's, which is most of the diagnosis.
bool ModuleForAddress(void* addr, char* out, size_t cap, uintptr_t* offset) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &mod) || !mod)
        return false;
    if (!GetModuleFileNameA(mod, out, (DWORD)cap)) return false;
    *offset = (uintptr_t)addr - (uintptr_t)mod;
    return true;
}

LONG WINAPI Filter(EXCEPTION_POINTERS* info) {
    // No allocation here; the path was formatted at startup.
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "w") != 0 || !f)
        return EXCEPTION_EXECUTE_HANDLER;

    time_t now = time(nullptr);
    char when[64]{};
    struct tm tmv{};
    if (localtime_s(&tmv, &now) == 0)
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmv);

    const DWORD code = info->ExceptionRecord->ExceptionCode;
    void* addr = info->ExceptionRecord->ExceptionAddress;

    fprintf(f, "loopcore crash report\n");
    fprintf(f, "time      %s\n", when);
    fprintf(f, "exception 0x%08lX  %s\n", (unsigned long)code, ExceptionName(code));
    fprintf(f, "address   %p\n", addr);

    char mod[MAX_PATH]{};
    uintptr_t off = 0;
    if (ModuleForAddress(addr, mod, sizeof(mod), &off))
        fprintf(f, "module    %s + 0x%llX\n", mod, (unsigned long long)off);
    else
        fprintf(f, "module    unknown\n");

    if (code == EXCEPTION_ACCESS_VIOLATION &&
        info->ExceptionRecord->NumberParameters >= 2) {
        const ULONG_PTR op = info->ExceptionRecord->ExceptionInformation[0];
        fprintf(f, "operation %s at %p\n",
                op == 0 ? "read" : op == 1 ? "write" : "execute",
                (void*)info->ExceptionRecord->ExceptionInformation[1]);
    }

    // Walking the stack needs dbghelp and symbols we may not have; the
    // return-address chain is still useful for placing the fault.
    fprintf(f, "\ncall stack (raw addresses)\n");
    CONTEXT* ctx = info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset    = ctx->Rip;   frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Offset = ctx->Rbp;   frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx->Rsp;   frame.AddrStack.Mode = AddrModeFlat;

    HANDLE proc = GetCurrentProcess();
    HANDLE thr  = GetCurrentThread();
    SymInitialize(proc, nullptr, TRUE);

    for (int i = 0; i < 32; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thr, &frame, ctx,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (!frame.AddrPC.Offset) break;

        char m[MAX_PATH]{};
        uintptr_t o = 0;
        if (ModuleForAddress((void*)frame.AddrPC.Offset, m, sizeof(m), &o)) {
            const char* leaf = strrchr(m, '\\');
            fprintf(f, "  %2d  %s + 0x%llX\n", i, leaf ? leaf + 1 : m,
                    (unsigned long long)o);
        } else {
            fprintf(f, "  %2d  %p\n", i, (void*)frame.AddrPC.Offset);
        }
    }
    SymCleanup(proc);

    fprintf(f, "\nSee loopcore.log in the same folder for what led up to this.\n");
    fclose(f);

    char msg[512];
    snprintf(msg, sizeof(msg),
             "loopcore hit an unrecoverable error and has to close.\n\n"
             "A report was written to:\n%s", g_logPath);
    MessageBoxA(nullptr, msg, "loopcore", MB_ICONERROR | MB_OK);

    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void InstallCrashHandler(const std::string& dir) {
    strncpy_s(g_dir, sizeof(g_dir), dir.c_str(), _TRUNCATE);
    snprintf(g_logPath, sizeof(g_logPath), "%s\\crash.log", g_dir);
    SetUnhandledExceptionFilter(Filter);
}

} // namespace lc
