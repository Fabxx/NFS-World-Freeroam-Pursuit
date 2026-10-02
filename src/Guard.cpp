// Guards against bad script calls right after LaunchPursuit.
#include "Guard.h"
#include "Game.h"
#include "Hooks.h"
#include "Log.h"
#include "Features.h"
#include <intrin.h>
#include <cstdio>
#include <cstring>

namespace Mod::Guard {
    constexpr uint32_t kDeleteNative = 0x686140;
    constexpr uint32_t kRadarRequest = 0x51CB40;
    constexpr ULONGLONG kRadarHoldMs = 1500;

    using Delete_t = int(__cdecl*)(uint32_t);
    using Radar_t = int(__cdecl*)(int*);
    static Delete_t g_origDelete = nullptr;
    static Radar_t g_origRadar = nullptr;
    static volatile LONG g_blocked = 0, g_radarSkipped = 0;
    static volatile ULONGLONG g_launchMs = 0;

    void NoteLaunch(ULONGLONG now) { g_launchMs = now; g_radarSkipped = 0; }

    static bool InExe(uint32_t p) {
        const uint32_t mb = static_cast<uint32_t>(ExeBase());
        return p >= mb && p < mb + 0x900000;
    }

    static bool LooksLikeObject(uint32_t obj) {
        uint32_t vt = 0, dtor = 0;
        if (obj < 0x10000 || (obj & 3) != 0) return false;
        if (!ReadU32(obj, vt) || !InExe(vt)) return false;
        return ReadU32(vt, dtor) && InExe(dtor);
    }

    static void Where(uint32_t a, char* out, size_t cap) {
        HMODULE m = nullptr;
        char name[MAX_PATH] = {};
        if (InExe(a)) { snprintf(out, cap, "exe:%08X", a - static_cast<uint32_t>(ExeBase()) + 0x400000); return; }
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(a)), &m) && m) {
            GetModuleFileNameA(m, name, MAX_PATH);
            const char* base = strrchr(name, '\\');
            base = base ? base + 1 : name;
            const uint32_t off = a - static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m));
            if (_stricmp(base, "gameplay.native.dll") == 0) snprintf(out, cap, "gn:%08X", off + 0x10000000);
            else snprintf(out, cap, "%s+%X", base, off);
            return;
        }
        snprintf(out, cap, "?%08X", a);
    }

    static void LogCallers(uint32_t ret, uint32_t frame) {
        char line[512], w[64];
        int len = 0;
        Where(ret, w, sizeof(w));
        len += snprintf(line + len, sizeof(line) - len, "ret %s | stack:", w);
        int found = 0;
        for (int i = 0; i < 512 && found < 10 && len < static_cast<int>(sizeof(line)) - 24; ++i) {
            uint32_t v = 0;
            if (!ReadU32(frame + 4 * i, v) || v < 0x10000) continue;
            HMODULE m = nullptr;
            if (!InExe(v) && !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                 reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(v)), &m)) continue;
            if (!InExe(v) && !m) continue;
            Where(v, w, sizeof(w));
            if (w[0] == '?' || strncmp(w, "NFSWorldPursuitProbe", 20) == 0) continue;
            len += snprintf(line + len, sizeof(line) - len, " %s", w);
            ++found;
        }
        LOG("[guard]   %s", line);
        (void)line;
    }

    static int __cdecl DeleteHook(uint32_t obj) {
        if (obj && Features::On("deleteguard") && !LooksLikeObject(obj)) {
            LONG n = InterlockedIncrement(&g_blocked);
            if (n <= 6) {
                uint32_t vt = 0;
                ReadU32(obj, vt);
                LOG("[guard] script delete of an invalid object 0x%08X (vt 0x%08X) skipped (#%ld)", obj, vt, n);
                if (n <= 2) LogCallers(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())),
                                       static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_AddressOfReturnAddress())));
            }
            else if (n == 7 || (n % 1000) == 0) LOG("[guard] ... %ld invalid deletes skipped so far", n);
            return 0;
        }
        return g_origDelete(obj);
    }

    static int __cdecl RadarHook(int* args) {
        const ULONGLONG t = g_launchMs;
        if (t && Features::On("radarguard")) {
            const ULONGLONG now = GetTickCount64();
            if (now >= t && now - t < kRadarHoldMs) {
                if (InterlockedIncrement(&g_radarSkipped) == 1) LOG("[guard] minimap tracker request held back right after LaunchPursuit");
                return 0;
            }
            if (g_radarSkipped) {
                LOG("[guard] minimap tracker requests resumed (%ld held back)", static_cast<long>(g_radarSkipped));
                g_radarSkipped = 0;
                g_launchMs = 0;
            }
        }
        return g_origRadar(args);
    }

    void Install() {
        g_origDelete = reinterpret_cast<Delete_t>(Addr(kDeleteNative));
        Hooks::Attach(reinterpret_cast<void**>(&g_origDelete), reinterpret_cast<void*>(DeleteHook), "script delete native (guard)");
        g_origRadar = reinterpret_cast<Radar_t>(Addr(kRadarRequest));
        Hooks::Attach(reinterpret_cast<void**>(&g_origRadar), reinterpret_cast<void*>(RadarHook), "minimap tracker request (guard)");
    }

    void Remove() {
        if (g_origDelete) Hooks::Detach(reinterpret_cast<void**>(&g_origDelete), reinterpret_cast<void*>(DeleteHook));
        if (g_origRadar) Hooks::Detach(reinterpret_cast<void**>(&g_origRadar), reinterpret_cast<void*>(RadarHook));
    }
}
