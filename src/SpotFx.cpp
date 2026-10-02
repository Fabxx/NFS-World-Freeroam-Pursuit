// 3D hiding-spot icons in cooldown (hiding-spot zones, natives 0x68B7F0 show / 0x68B800 hide).
#include "SpotFx.h"
#include "Game.h"
#include "Hooks.h"
#include "Log.h"
#include "Features.h"
#include "Pursuit.h"
#include "MapIcons.h"
#include "MarkerFx.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace Mod::SpotFx {
    constexpr uint32_t kZoneCtor = 0x6A2130;
    constexpr uint32_t kZoneVtable = 0xBB9840;
    constexpr uint32_t kZoneOnNative = 0x68B800;
    constexpr uint32_t kZoneOffNative = 0x68B7F0;
    constexpr int kMaxZones = 512;

    static uint32_t g_zone[kMaxZones] = {};
    static uint8_t g_scriptState[kMaxZones] = {};
    static bool g_learned[kMaxZones] = {};
    static int g_learnedCount = 0;
    static ULONGLONG g_burstMs = 0;
    static int g_burstIdx[kMaxZones];
    static int g_burstCount = 0;
    static uint32_t g_group[kMaxZones] = {};
    static volatile LONG g_count = 0;
    static bool g_on = false;

    static uint32_t g_scriptZone[64] = {};
    static int g_scriptCount = 0;
    static bool g_inOurCall = false;

    static void NoteScript(uint32_t z) {
        if (g_inOurCall || !Pursuit::IsActive()) return;
        for (int i = 0; i < g_scriptCount; ++i) if (g_scriptZone[i] == z) return;
        if (g_scriptCount < 64) g_scriptZone[g_scriptCount++] = z;
    }
    static bool ScriptOwned(uint32_t z) {
        for (int i = 0; i < g_scriptCount; ++i) if (g_scriptZone[i] == z) return true;
        return false;
    }

    using Ctor_t = void*(__thiscall*)(void*, int);
    static Ctor_t g_origCtor = nullptr;
    using Native_t = int(__cdecl*)(int);
    static Native_t g_origOn = nullptr, g_origOff = nullptr;

    static int IndexOf(uint32_t z) {
        const LONG n = g_count < kMaxZones ? g_count : kMaxZones;
        for (LONG i = 0; i < n; ++i) if (g_zone[i] == z) return static_cast<int>(i);
        return -1;
    }

    static void* __fastcall CtorHook(void* self, void* /*edx*/, int group) {
        void* r = g_origCtor(self, group);
        const uint32_t z = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
        if (IndexOf(z) < 0) {
            LONG i = InterlockedIncrement(&g_count) - 1;
            if (i < kMaxZones) { g_zone[i] = z; g_group[i] = static_cast<uint32_t>(group); }
        }
        return r;
    }

    static uint32_t Fsm() { return ReadU32Or(Addr(Ida::Fsm), 0); }

    static const char* kSpotsHeader = "# NFSWorldPursuitProbe hiding-spot zones v2";

    static std::string SpotsPath() { return ModuleDir() + "NFSWorldPursuitProbe_spots.txt"; }

    static void SaveLearned() {
        FILE* f = nullptr;
        if (fopen_s(&f, SpotsPath().c_str(), "w") != 0 || !f) return;
        fprintf(f, "%s\n", kSpotsHeader);
        for (int i = 0; i < kMaxZones; ++i) if (g_learned[i]) fprintf(f, "%d\n", i);
        fclose(f);
    }

    static void LoadLearned() {
        FILE* f = nullptr;
        if (fopen_s(&f, SpotsPath().c_str(), "r") != 0 || !f) return;
        char line[128] = {};
        if (fgets(line, sizeof(line), f) && strncmp(line, kSpotsHeader, strlen(kSpotsHeader)) == 0) {
            int i = 0;
            while (fscanf_s(f, "%d", &i) == 1) if (i >= 0 && i < kMaxZones && !g_learned[i]) { g_learned[i] = true; ++g_learnedCount; }
        }
        fclose(f);
        LOG("[spots] %d hiding-spot zone(s) from %s", g_learnedCount, SpotsPath().c_str());
    }

    // Built-in layout: the world creates the pursuit-breaker zones first, then the hiding-spot zones,
    // one per map icon. Written to NFSWorldPursuitProbe_spots.txt the first time the map is ready.
    static void EnsureSpots() {
        const int spots = MapIcons::CountHidingSpots(), breakers = MapIcons::CountBreakers();
        if (spots <= 0 || breakers <= 0 || breakers + spots > g_count) return;
        if (g_learnedCount == spots) return;
        memset(g_learned, 0, sizeof(g_learned));
        g_learnedCount = 0;
        for (int i = breakers; i < breakers + spots && i < kMaxZones; ++i) { g_learned[i] = true; ++g_learnedCount; }
        SaveLearned();
        LOG("[spots] built-in layout: hiding-spot zones %d-%d written to %s", breakers, breakers + spots - 1, SpotsPath().c_str());
    }

    static bool RealCooldown() {
        uint32_t mgr = 0, p = 0, cd = 0;
        return ReadU32(Addr(Ida::CopMgrPtr), mgr) && mgr && ReadU32(mgr + 0x1E0, p) && p && ReadU32(p + 0x1A4, cd) && (cd & 0xFF);
    }

    static void NoteRealBurst(int idx) {
        if (idx < 0 || Pursuit::IsActive() || Fsm() != 4 || !RealCooldown()) return;
        const ULONGLONG now = GetTickCount64();
        if (now - g_burstMs > 300) g_burstCount = 0;
        g_burstMs = now;
        if (g_burstCount < kMaxZones) g_burstIdx[g_burstCount++] = idx;
        const int spots = MapIcons::CountHidingSpots();
        if (spots >= 10 && g_burstCount >= spots) {
            const int first = g_burstCount - spots;
            bool same = g_learnedCount == spots;
            for (int k = first; k < g_burstCount && same; ++k) same = g_learned[g_burstIdx[k]];
            if (same) return;
            memset(g_learned, 0, sizeof(g_learned));
            g_learnedCount = 0;
            for (int k = first; k < g_burstCount; ++k) if (!g_learned[g_burstIdx[k]]) { g_learned[g_burstIdx[k]] = true; ++g_learnedCount; }
            SaveLearned();
            LOG("[spots] learned %d hiding-spot zone(s) from the real cooldown", g_learnedCount);
        }
    }

    static bool IsSpotZone(int i) { return i >= 0 && i < kMaxZones && g_learned[i]; }
    static int g_blockedIdx[kMaxZones];
    static int g_blockedCount = 0;

    // During our pursuit the script still runs its freeroam logic and hides the hiding-spot icons
    // near the player (the nearest ones, i.e. the visible ones). Those calls are dropped.
    static bool BlockScript(int zone, const char* what) {
        if (g_inOurCall || !Pursuit::IsActive() || !Features::On("spot3d")) return false;
        const int idx = IndexOf(static_cast<uint32_t>(zone));
        if (!IsSpotZone(idx)) return false;
        for (int i = 0; i < g_blockedCount; ++i) if (g_blockedIdx[i] == idx) return true;
        if (g_blockedCount < kMaxZones) g_blockedIdx[g_blockedCount++] = idx;
        LOG("[spots] script %s on hiding-spot zone #%d ignored during our pursuit", what, idx);
        (void)what;
        return true;
    }

    static int __cdecl OnHook(int zone) {
        if (BlockScript(zone, "hide")) return zone;
        if (!g_inOurCall) {
            const int idx = IndexOf(static_cast<uint32_t>(zone));
            if (idx >= 0 && !Pursuit::IsActive()) g_scriptState[idx] = 1;
            NoteRealBurst(idx);
            LOG("[spots] script: zone #%d 0x%08X ON%s", IndexOf(static_cast<uint32_t>(zone)), zone, Pursuit::IsActive() ? " (our pursuit)" : "");
            NoteScript(static_cast<uint32_t>(zone));
        }
        return g_origOn(zone);
    }
    static int __cdecl OffHook(int zone) {
        if (BlockScript(zone, "show")) return zone;
        if (!g_inOurCall) {
            const int idx = IndexOf(static_cast<uint32_t>(zone));
            if (idx >= 0 && !Pursuit::IsActive()) g_scriptState[idx] = 2;
            NoteRealBurst(idx);
            LOG("[spots] script: zone #%d 0x%08X OFF%s", IndexOf(static_cast<uint32_t>(zone)), zone, Pursuit::IsActive() ? " (our pursuit)" : "");
            NoteScript(static_cast<uint32_t>(zone));
        }
        return g_origOff(zone);
    }

    static bool IsZone(uint32_t z) {
        uint32_t vt = 0;
        return z && ReadU32(z, vt) && vt == Addr32(kZoneVtable);
    }

    static int SwitchSpots(bool show) {
        const LONG n = g_count < kMaxZones ? g_count : kMaxZones;
        int ok = 0;
        for (LONG i = 0; i < n; ++i) {
            const uint32_t z = g_zone[i];
            if (!IsSpotZone(static_cast<int>(i)) || !IsZone(z) || ScriptOwned(z)) continue;
            g_inOurCall = true;
            __try { (show ? g_origOff : g_origOn)(static_cast<int>(z)); ++ok; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            g_inOurCall = false;
        }
        return ok;
    }

    // The first time after the world loads, a hiding-spot icon only appears after it went through a
    // hide/show cycle (first pursuit: no icons, from the second one on: icons). Do that cycle once
    // when the pursuit starts, so the first cooldown already shows them.
    static uint32_t g_prewarmedZone = 0;

    static void Prewarm() {
        uint32_t first = 0;
        for (int i = 0; i < kMaxZones && i < g_count; ++i) if (IsSpotZone(i)) { first = g_zone[i]; break; }
        if (!first || first == g_prewarmedZone || !Features::On("spot3d")) return;
        const int shown = SwitchSpots(true);
        const int hidden = SwitchSpots(false);
        g_prewarmedZone = first;
        LOG("[spots] hiding-spot icons prewarmed (%d shown, %d hidden)", shown, hidden);
        (void)shown; (void)hidden;
    }

    void PursuitStarted() {
        g_scriptCount = 0;
        g_blockedCount = 0;
        EnsureSpots();
        Prewarm();
    }

    void SetCooldown(bool on) {
        if (!Features::On("spot3d")) return;
        if (!on) { Off(); return; }
        if (g_on) return;
        EnsureSpots();
        struct Ctx { int ok; } ctx = { 0 };
        const int markers = MarkerFx::FlagSpotsTurnedOnBy([](void* c) { static_cast<Ctx*>(c)->ok = SwitchSpots(true); }, &ctx);
        const int ok = ctx.ok;
        g_on = true;
        LOG("[spots] %d hiding-spot marker(s) turned on (left alone by the event-marker hider)", markers);
        (void)markers;
        LOG("[spots] cooldown: %d hiding-spot icon(s) shown (%s, %ld zones, %d left to the script)", ok,
            g_learnedCount ? "zones from the spots file" : "no hiding-spot zones known", static_cast<long>(g_count), g_scriptCount);
        (void)ok;
    }

    void Off() {
        if (!g_on) return;
        int ok = SwitchSpots(false);
        g_on = false;
        LOG("[spots] %d hiding-spot icon(s) hidden", ok);
        (void)ok;
    }

    void Install() {
        LoadLearned();
        g_origCtor = reinterpret_cast<Ctor_t>(Addr(kZoneCtor));
        Hooks::Attach(reinterpret_cast<void**>(&g_origCtor), reinterpret_cast<void*>(CtorHook), "hiding-spot zone ctor");
        g_origOn = reinterpret_cast<Native_t>(Addr(kZoneOnNative));
        g_origOff = reinterpret_cast<Native_t>(Addr(kZoneOffNative));
        Hooks::Attach(reinterpret_cast<void**>(&g_origOn), reinterpret_cast<void*>(OnHook), "zone models on");
        Hooks::Attach(reinterpret_cast<void**>(&g_origOff), reinterpret_cast<void*>(OffHook), "zone models off");
    }

    void Remove() {
        Hooks::Detach(reinterpret_cast<void**>(&g_origCtor), reinterpret_cast<void*>(CtorHook));
        Hooks::Detach(reinterpret_cast<void**>(&g_origOn), reinterpret_cast<void*>(OnHook));
        Hooks::Detach(reinterpret_cast<void**>(&g_origOff), reinterpret_cast<void*>(OffHook));
    }
}
