#ifdef _DEBUG
#include "Log.h"
#include "Game.h"
#include "Pursuit.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace Mod::Log {

    static FILE* g_file = nullptr;
    static std::mutex g_mutex;

    void Open() {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_file) return;
        std::string path = ModuleDir() + "NFSWorldPursuitProbe.log";
        if (fopen_s(&g_file, path.c_str(), "w") != 0) g_file = nullptr;
    }

    void Close() {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_file) { fclose(g_file); g_file = nullptr; }
    }

    void Write(const char* fmt, ...) {
        char body[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(body, sizeof(body), fmt, ap);
        va_end(ap);
        SYSTEMTIME st;
        GetLocalTime(&st);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_file) return;
        fprintf(g_file, "[%02u:%02u:%02u.%03u] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);
        fflush(g_file);
    }

    // ---------------------------------------------------------- crash logger
    static PVOID g_veh = nullptr;
    static volatile LONG g_crashLines = 0;

    static LONG CALLBACK CrashHandler(PEXCEPTION_POINTERS ep) {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (code != EXCEPTION_ACCESS_VIOLATION && code != 0x80000003 /* int3: EASharp unhandled managed exception */ &&
            code != EXCEPTION_STACK_OVERFLOW && code != 0xC0000374 /* heap corruption */)
            return EXCEPTION_CONTINUE_SEARCH;
        if (!Pursuit::InResultsWindow() || InterlockedIncrement(&g_crashLines) > 8) return EXCEPTION_CONTINUE_SEARCH;
        // our own guarded reads fault on purpose
        HMODULE self = nullptr, at = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&CrashHandler), &self);
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(ep->ExceptionRecord->ExceptionAddress), &at);
        if (at && at == self) return EXCEPTION_CONTINUE_SEARCH;

        uintptr_t mb = ExeBase();
        auto ida = [mb](uint32_t a) -> uint32_t { return (a >= mb + 0x1000 && a < mb + 0x900000) ? a - static_cast<uint32_t>(mb) + 0x400000 : 0; };
        CONTEXT* c = ep->ContextRecord;
        uint32_t accAddr = ep->ExceptionRecord->NumberParameters >= 2 ? static_cast<uint32_t>(ep->ExceptionRecord->ExceptionInformation[1]) : 0;
        char stack[400];
        int len = 0, found = 0;
        stack[0] = 0;
        for (int i = 0; i < 1024 && found < 16; ++i) {
            uint32_t v = 0;
            if (!ReadU32(c->Esp + 4 * i, v)) break;
            if (uint32_t a = ida(v)) { len += sprintf_s(stack + len, sizeof(stack) - len, " %08X", a); ++found; }
        }
        Write("[crash] code 0x%08X eip 0x%08X (IDA 0x%08X) addr 0x%08X tid %lu | stack (IDA):%s", code, c->Eip, ida(c->Eip),
            accAddr, GetCurrentThreadId(), stack);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void InstallCrashLogger() { if (!g_veh) g_veh = AddVectoredExceptionHandler(1, CrashHandler); }
    void RemoveCrashLogger() { if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; } }
}
#endif
