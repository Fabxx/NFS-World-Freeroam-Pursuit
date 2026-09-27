#ifdef _DEBUG
// Debug-only: every EA# script native registered in sub_6ACE60 (addresses from
// round 75) gets a generic thunk that logs its FIRST call with a timestamp.
// Natives first seen right after F7 are what the event script does at launch
// (e.g. hiding the in-world event markers), compared with our F9 pursuit.
#include "Log.h"
#include "Game.h"
#include "Pursuit.h"
#include <cstring>
#include <detours.h>

namespace Mod::Log {

    static const uint32_t kNatives[] = {
#include "ScriptBindings.inc"
    };
    constexpr int kNativeCount = static_cast<int>(sizeof(kNatives) / sizeof(kNatives[0]));

    static void* g_nativeTramp[kNativeCount];
    static void* g_nativeThunk[kNativeCount];
    static volatile LONG g_nativeSeen[kNativeCount];
    static uint8_t* g_thunkMem = nullptr;

    // Capture window (armed by F7): every call, deduplicated by (native, object vtables, a2, a3).
    static volatile ULONGLONG g_captureUntilMs = 0;
    constexpr int kCapSlots = 8192, kCapMaxLines = 3000;
    static uint32_t g_capSeen[kCapSlots];
    static volatile LONG g_capLines = 0;

    void ArmNativeCapture(uint32_t ms) {
        memset(g_capSeen, 0, sizeof(g_capSeen));
        g_capLines = 0;
        g_captureUntilMs = GetTickCount64() + ms;
        Write("[native] capture armed for %u ms", ms);
    }

    static uint32_t ToIda(uint32_t v) {
        const uint32_t mb = static_cast<uint32_t>(ExeBase());
        return (v >= mb && v < mb + 0x900000) ? v - mb + 0x400000 : 0;
    }

    // Vtable (IDA) of the object at p, 0 if p is not an object of the exe.
    static uint32_t VtOf(uint32_t p) {
        uint32_t vt = 0;
        if (p < 0x10000 || !ReadU32(p, vt)) return 0;
        return ToIda(vt);
    }

    static uint32_t Mix(uint32_t h, uint32_t v) { h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2); return h; }

    static bool CapNew(uint32_t h) {
        if (!h) h = 1;
        uint32_t slot = h % kCapSlots;
        for (int probe = 0; probe < 32; ++probe, slot = (slot + 1) % kCapSlots) {
            if (g_capSeen[slot] == h) return false;
            if (!g_capSeen[slot]) { g_capSeen[slot] = h; return true; }
        }
        return false;
    }

    // f[0] = idx, f[1] = return address, f[2..5] = first cdecl arguments.
    static void __stdcall NativeLog(const uint32_t* f) {
        uint32_t idx = f[0], ret = f[1];
        if (idx >= static_cast<uint32_t>(kNativeCount)) return;
        uint32_t retIda = ToIda(ret) ? ToIda(ret) : ret;
        uint32_t fsm = ReadU32Or(Addr(Ida::Fsm), 0xFFFFFFFF);
        if (!InterlockedExchange(&g_nativeSeen[idx], 1))
            Write("[native] first call #%u IDA 0x%06X  ret IDA 0x%08X  FSM=%u %s", idx, kNatives[idx], retIda, fsm,
                  Pursuit::IsActive() ? "[OUR pursuit]" : "");
        if (GetTickCount64() >= g_captureUntilMs || g_capLines >= kCapMaxLines) return;
        if (ToIda(ret)) return;   // called by the game's C++, not by the script (JIT code lives outside the exe)
        uint32_t a1 = f[2], a2 = f[3], a3 = f[4], a4 = f[5], in1 = 0;
        uint32_t vt1 = VtOf(a1), vtIn = 0;
        if (a1 >= 0x10000 && ReadU32(a1 + 4, in1)) vtIn = VtOf(in1);
        uint32_t h = Mix(Mix(Mix(Mix(Mix(idx, vt1), vtIn), a2), a3), retIda);
        static SRWLOCK lock = SRWLOCK_INIT;
        AcquireSRWLockExclusive(&lock);
        bool fresh = CapNew(h);
        ReleaseSRWLockExclusive(&lock);
        if (!fresh) return;
        InterlockedIncrement(&g_capLines);
        Write("[cap] #%u 0x%06X a1=%08X(vt %06X, +4 vt %06X) a2=%08X a3=%08X a4=%08X ret %08X FSM=%u", idx, kNatives[idx],
              a1, vt1, vtIn, a2, a3, a4, retIda, fsm);
    }

    // Thunk: push idx; jmp NativeCommon.  Stack on entry here: [esp] = idx, [esp+4] = ret, [esp+8..] = args.
    static __declspec(naked) void NativeCommon() {
        __asm {
            pushfd
            pushad
            lea  eax, [esp + 36]          // -> idx, ret, a1, a2, ...
            push eax
            call NativeLog
            popad
            popfd
            xchg eax, dword ptr [esp]     // eax = idx, [esp] = caller's eax
            mov  eax, dword ptr g_nativeTramp[eax * 4]
            xchg eax, dword ptr [esp]     // eax restored, [esp] = trampoline
            ret
        }
    }

    void InstallScriptProbe() {
        g_thunkMem = static_cast<uint8_t*>(VirtualAlloc(nullptr, kNativeCount * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!g_thunkMem) { Write("[native] thunk memory FAILED"); return; }
        int ok = 0, failed = 0;
        for (int i = 0; i < kNativeCount; ++i) {
            uint8_t* t = g_thunkMem + i * 16;
            t[0] = 0x68; *reinterpret_cast<uint32_t*>(t + 1) = static_cast<uint32_t>(i);
            t[5] = 0xE9; *reinterpret_cast<int32_t*>(t + 6) =
                static_cast<int32_t>(reinterpret_cast<uintptr_t>(&NativeCommon) - reinterpret_cast<uintptr_t>(t + 10));
            g_nativeThunk[i] = t;
            g_nativeTramp[i] = reinterpret_cast<void*>(Addr(kNatives[i]));
            // One transaction per target: a function too short to patch only fails itself.
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            if (DetourAttach(&g_nativeTramp[i], g_nativeThunk[i]) == NO_ERROR && DetourTransactionCommit() == NO_ERROR) ++ok;
            else { DetourTransactionAbort(); g_nativeTramp[i] = nullptr; g_nativeThunk[i] = nullptr; ++failed; }
        }
        FlushInstructionCache(GetCurrentProcess(), g_thunkMem, kNativeCount * 16);
        Write("[native] %d script natives hooked, %d skipped (too short)", ok, failed);
    }

    void RemoveScriptProbe() {
        for (int i = 0; i < kNativeCount; ++i) {
            if (!g_nativeThunk[i] || !g_nativeTramp[i]) continue;
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourDetach(&g_nativeTramp[i], g_nativeThunk[i]);
            DetourTransactionCommit();
        }
    }
}
#endif
