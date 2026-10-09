// Live pursuit stats from the HUD (cops deployed/disabled/rammed, cost to state).
#include "Stats.h"
#include "Game.h"
#include "Hooks.h"
#include "Pursuit.h"
#include "RepFlash.h"
#include "Log.h"
#include <cstring>

namespace Mod::Stats {
    constexpr uint32_t kHudUpdate = 0x48CCB0;
    static volatile LONG g_deployed = 0, g_disabled = 0, g_rammed = 0, g_cost = 0, g_valid = 0;
    static uint32_t g_orig = 0;

    static void __stdcall OnHud(uint32_t hud) {
        if (!Pursuit::IsActive()) return;
        uint32_t b = 0, e = 0, dep = 0, dis = 0, ram = 0, costBits = 0;
        if (!ReadU32(hud + 120, b) || !ReadU32(hud + 124, e) || !b || e < b + 64) return;
        if (!ReadU32(b + 24, dep) || !ReadU32(b + 28, dis) || !ReadU32(b + 32, ram) || !ReadU32(b + 36, costBits)) return;
        float cost = 0.0f;
        memcpy(&cost, &costBits, 4);
        if (dep > 100 || dis > 100 || ram > 100 || !(cost >= 0.0f && cost < 1.0e7f)) return;
        if (static_cast<LONG>(dep) > g_deployed) g_deployed = static_cast<LONG>(dep);
        if (static_cast<LONG>(dis) > g_disabled) g_disabled = static_cast<LONG>(dis);
        if (static_cast<LONG>(ram) > g_rammed) g_rammed = static_cast<LONG>(ram);
        if (static_cast<LONG>(cost) > g_cost) g_cost = static_cast<LONG>(cost);
        if (::Mod::Log::Enabled()) {
            static LONG s_last[4] = {};
            LONG now[4] = { g_deployed, g_disabled, g_rammed, g_cost };
            if (memcmp(now, s_last, sizeof(now)) != 0) {
                memcpy(s_last, now, sizeof(now));
                LOG("[stats] cops deployed %ld, cop info %ld / %ld, cost to state %ld", now[0], now[1], now[2], now[3]);
            }
        }
        g_valid = 1;
        RepFlash::Poll();
    }

    static __declspec(naked) void HudStub() {
        __asm {
            pushfd
            pushad
            push dword ptr [esp + 40]
            call OnHud
            popad
            popfd
            jmp  dword ptr [g_orig]
        }
    }

    void Reset() { g_deployed = g_disabled = g_rammed = g_cost = g_valid = 0; }

    Snapshot Get() {
        return Snapshot{ static_cast<uint32_t>(g_deployed), static_cast<uint32_t>(g_disabled), static_cast<uint32_t>(g_rammed),
                         static_cast<uint32_t>(g_cost), g_valid != 0 };
    }

    void Install() {
        g_orig = Addr32(kHudUpdate);
        Hooks::Attach(reinterpret_cast<void**>(&g_orig), reinterpret_cast<void*>(HudStub), "pursuit HUD update (stats)");
    }

    void Remove() { Hooks::Detach(reinterpret_cast<void**>(&g_orig), reinterpret_cast<void*>(HudStub)); }
}
