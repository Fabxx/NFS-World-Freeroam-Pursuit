#include "Game.h"
#include <atomic>

namespace Mod {

    static uintptr_t g_exeBase = 0;
    static std::atomic<DWORD> g_gameThreadId{ 0 };

    uintptr_t ExeBase() {
        if (!g_exeBase) g_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        return g_exeBase;
    }

    bool ReadU32(uintptr_t address, uint32_t& out) {
        __try { out = *reinterpret_cast<volatile uint32_t*>(address); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    uint32_t ReadU32Or(uintptr_t address, uint32_t fallback) {
        uint32_t v = 0;
        return ReadU32(address, v) ? v : fallback;
    }

    uint32_t __declspec(noinline) CallAny1(uint32_t fn, uint32_t ecxVal, uint32_t a1) {
        uint32_t result = 0;
        __asm {
            mov  esi, esp
            push a1
            mov  ecx, ecxVal
            mov  eax, fn
            call eax
            mov  esp, esi
            mov  result, eax
        }
        return result;
    }

    uint32_t __declspec(noinline) CallAny2(uint32_t fn, uint32_t ecxVal, uint32_t a1, uint32_t a2) {
        uint32_t result = 0;
        __asm {
            mov  esi, esp
            push a2
            push a1
            mov  ecx, ecxVal
            mov  eax, fn
            call eax
            mov  esp, esi
            mov  result, eax
        }
        return result;
    }

    bool TryCallAny1(uint32_t fn, uint32_t ecxVal, uint32_t a1, uint32_t& out) {
        out = 0;
        __try { out = CallAny1(fn, ecxVal, a1); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    struct FindWindowCtx { DWORD pid; HWND best; LONG bestArea; };

    static BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM lp) {
        auto* ctx = reinterpret_cast<FindWindowCtx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != ctx->pid || !IsWindowVisible(hwnd)) return TRUE;
        RECT rc{};
        if (!GetClientRect(hwnd, &rc)) return TRUE;
        LONG area = (rc.right - rc.left) * (rc.bottom - rc.top);
        if (area > ctx->bestArea) { ctx->bestArea = area; ctx->best = hwnd; }
        return TRUE;
    }

    void ResolveGameThread() {
        if (g_gameThreadId.load(std::memory_order_relaxed) != 0) return;
        FindWindowCtx ctx{ GetCurrentProcessId(), nullptr, 0 };
        EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&ctx));
        if (ctx.best && ctx.bestArea > 0)
            g_gameThreadId.store(GetWindowThreadProcessId(ctx.best, nullptr), std::memory_order_release);
    }

    bool OnGameThread() {
        DWORD tid = g_gameThreadId.load(std::memory_order_acquire);
        return tid != 0 && GetCurrentThreadId() == tid;
    }

    bool GameHasFocus() {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        return pid == GetCurrentProcessId();
    }

    std::string ModuleDir() {
        HMODULE self = nullptr;
        char buf[MAX_PATH] = {};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&ModuleDir), &self) && self && GetModuleFileNameA(self, buf, MAX_PATH)) {
            std::string full(buf);
            size_t slash = full.find_last_of("\\/");
            if (slash != std::string::npos) return full.substr(0, slash + 1);
        }
        return std::string();
    }
}
