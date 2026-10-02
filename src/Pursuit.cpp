// Pursuit lifecycle: LaunchPursuit, cops, HUD/settings, cooldown, end and results screen.
#include "Pursuit.h"
#include "Game.h"
#include "Hooks.h"
#include "Music.h"
#include "Features.h"
#include "Results.h"
#include "Guard.h"
#include "SpotFx.h"
#include "Stats.h"
#include "MapIcons.h"
#include "MarkerFx.h"
#include "Log.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace Mod::Pursuit {
    constexpr int32_t   kMinHeat = 1, kMaxHeat = 5;
    constexpr ULONGLONG kStep1DelayMs           = 650;
    constexpr ULONGLONG kUiStepDelayMs          = 750;
    constexpr ULONGLONG kGadgetReplayAgainMs    = 1500;
    constexpr ULONGLONG kEndAfterPursuitGoneMs  = 1000;
    constexpr ULONGLONG kResultsTimeoutMs       = 60000;
    constexpr ULONGLONG kGoalFollowupMs         = 1500;
    constexpr ULONGLONG kResultsWindowAfterExit = 30000;
    constexpr ULONGLONG kWatchPeriodMs          = 200;
    constexpr uint32_t  kFsmFreeroam = 1, kFsmPursuit = 4;
    constexpr int32_t   kPursuitMode = 12;
    constexpr uint32_t  kPowerupPageOffset = 168;
    constexpr int       kMaxCops = 64, kMaxTracked = 32, kMaxGadgets = 16;

    constexpr uint32_t kCopMgrPursuit = 0x1E0, kCandidates = 0x120, kCandidateCount = 0x128;
    constexpr uint32_t kManaged = 0x5C, kManagedCount = 0x64;
    constexpr int kAdoptVt = 28;

    static uint32_t g_cfgSlots[4] = { 0x306A1451, 0xF637CC39, 0x42ED6A19, 0x794FC606 };
    static uint32_t g_cfgPage = 3;
    static char g_cfgGadget[kMaxGadgets][48] = {};
    static int g_cfgGadgetShow[kMaxGadgets] = {};
    static int g_cfgGadgetCount = 0;

    static void AddGadget(const char* name, int show) {
        for (int i = 0; i < g_cfgGadgetCount; ++i)
            if (strcmp(g_cfgGadget[i], name) == 0) { g_cfgGadgetShow[i] = show; return; }
        if (g_cfgGadgetCount >= kMaxGadgets) return;
        strcpy_s(g_cfgGadget[g_cfgGadgetCount], name);
        g_cfgGadgetShow[g_cfgGadgetCount++] = show;
    }

    static void LoadConfig() {
        static const char* const kDefaultGadgets[] = { "PursuitBarGadget", "PursuitOutrunRankingHUD", "TickerGadget",
                                                       "InstrumentCluster", "PowerUp", "PowerUpBuffBar" };
        FILE* f = nullptr;
        std::string path = ModuleDir() + "NFSWorldPursuitProbe_recording.txt";
        bool fileGadgets = false;
        if (fopen_s(&f, path.c_str(), "r") == 0 && f) {
            char line[160];
            while (fgets(line, sizeof(line), f)) {
                unsigned a = 0, b = 0, c = 0, d = 0; int page = -1, show = 0; char name[48] = {};
                if (sscanf_s(line, "settings %x %x %x %x page %d", &a, &b, &c, &d, &page) == 5) {
                    g_cfgSlots[0] = a; g_cfgSlots[1] = b; g_cfgSlots[2] = c; g_cfgSlots[3] = d;
                    if (page >= 0) g_cfgPage = static_cast<uint32_t>(page);
                }
                else if (sscanf_s(line, "gadget %47s %d", name, (unsigned)sizeof(name), &show) == 2 &&
                         _stricmp(name, "PostRace") != 0) {
                    AddGadget(name, show);
                    fileGadgets = true;
                }
            }
            fclose(f);
            LOG("[config] loaded %s", path.c_str());
        }
        if (!fileGadgets) for (const char* n : kDefaultGadgets) AddGadget(n, 1);
        LOG("[config] settings %08X %08X %08X %08X page %u, %d gadget(s)", g_cfgSlots[0], g_cfgSlots[1],
            g_cfgSlots[2], g_cfgSlots[3], g_cfgPage, g_cfgGadgetCount);
    }

    static std::atomic<bool> g_hitRequested{ false };
    static ULONGLONG g_step1DueMs = 0;
    static ULONGLONG g_uiStepDueMs = 0;
    static bool g_active = false;
    static bool g_uiEntered = false;
    static bool g_sawAIPursuit = false;
    static bool g_cooldown = false;
    constexpr uint32_t kPursuitCooldown = 0x1A4;
    static ULONGLONG g_aiPursuitGoneMs = 0;
    static bool g_resultsPhase = false;
    static ULONGLONG g_resultsStartMs = 0;
    static uint32_t g_exitCallsAtResults = 0;
    static ULONGLONG g_gadgetReplayDueMs = 0;
    static ULONGLONG g_nextWatchMs = 0;
    static std::atomic<ULONGLONG> g_resultsWindowUntilMs{ 0 };
    static std::atomic<bool> g_resultsWindowFlag{ false };
    static ULONGLONG g_statStartMs = 0, g_statEndMs = 0;
    static uint32_t g_statMaxCops = 0;
    static bool g_statBusted = false;
    static float g_statHeat = 0.0f;
    static uint8_t g_exitDummyThis[64] = {};

    using ModeGetter_t = int(__thiscall*)(void*);
    static ModeGetter_t g_origModeGetter = nullptr;
    static std::atomic<bool> g_modeOverride{ false };

    static int __fastcall ModeGetterHook(void* self, void*) {
        if (g_modeOverride.load(std::memory_order_relaxed) &&
            reinterpret_cast<uintptr_t>(self) == *reinterpret_cast<volatile uintptr_t*>(Addr(Ida::ModeCtx) + 0xF0))
            return kPursuitMode;
        return g_origModeGetter(self);
    }

    static bool CallSetFsm(uint32_t index) {
        __try {
            using SetState_t = uint32_t(__thiscall*)(void*, int, uint32_t);
            reinterpret_cast<SetState_t>(Addr(Ida::FsmSetState))(reinterpret_cast<void*>(Addr(Ida::Fsm)), 0, index);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static uint32_t FsmIndex() { return ReadU32Or(Addr(Ida::Fsm), 0xFFFFFFFF); }

    static char SetGadgetVisible(const char* name, int show) {
        __try { return reinterpret_cast<char(__stdcall*)(const char*, int)>(Addr(Ida::SetGadgetVisible))(name, show); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }

    static void ShowPursuitGadgets() {
        bool all = Features::On("gadgets");
        for (int i = 0; i < g_cfgGadgetCount; ++i) {
            if (!all && _strnicmp(g_cfgGadget[i], "PowerUp", 7) != 0) continue;
            char r = SetGadgetVisible(g_cfgGadget[i], g_cfgGadgetShow[i]);
            LOG("[gadgets] SetGadgetVisible(\"%s\", %d) -> %d", g_cfgGadget[i], g_cfgGadgetShow[i], (int)r);
            (void)r;
        }
    }

    static bool LaunchPursuit(int32_t heat) {
        uintptr_t gp = reinterpret_cast<uintptr_t>(GetModuleHandleA(GameplayNative::Module));
        if (!gp) return false;
        uintptr_t slot = gp + (GameplayNative::LaunchPursuitSlot - GameplayNative::IdaBase);
        uint32_t target = 0;
        if (!ReadU32(slot, target) || !target) return false;
        __try {
            reinterpret_cast<int32_t(__cdecl*)(int32_t, int32_t)>(static_cast<uintptr_t>(target))(heat, 1);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    struct Settings { uint32_t obj = 0; uint32_t slots[4] = {}; uint32_t pm = 0; uint32_t page = 0xFFFFFFFF; bool ok = false; };

    static Settings g_saved;
    static bool g_settingsApplied = false;
    static uint32_t g_freeroamPage = 0;
    static uint32_t g_pmTouched[4] = {}, g_pmOrigPage[4] = {};
    static int g_pmTouchedCount = 0;
    static ULONGLONG g_restoreRefreshDueMs = 0;
    static int g_restoreRefreshStep = 0;

    static Settings ReadSettings() {
        Settings s;
        if (!ReadU32(Addr(Ida::EventSettings), s.obj) || !s.obj) return s;
        for (int i = 0; i < 4; ++i) ReadU32(s.obj + i * 4, s.slots[i]);
        if (ReadU32(Addr(Ida::PowerupMgr), s.pm) && s.pm) ReadU32(s.pm + kPowerupPageOffset, s.page);
        s.ok = true;
        return s;
    }

    static void WriteU32(uint32_t address, uint32_t value) {
        __try { *reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(address)) = value; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void SetPowerupMgrPage(uint32_t pm, uint32_t curPage, uint32_t page) {
        bool known = false;
        for (int i = 0; i < g_pmTouchedCount; ++i) if (g_pmTouched[i] == pm) known = true;
        if (!known && g_pmTouchedCount < 4) { g_pmTouched[g_pmTouchedCount] = pm; g_pmOrigPage[g_pmTouchedCount++] = curPage; }
        WriteU32(pm + kPowerupPageOffset, page);
    }

    static void SetPowerupPageNative(uint32_t page) {
        __try { reinterpret_cast<int(__stdcall*)(int)>(Addr(Ida::SetPowerupPage))(static_cast<int>(page)); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void RefreshPowerupGadget() {
        uint32_t screen = 0, hash = 0, gadget = 0;
        __try { screen = reinterpret_cast<uint32_t(__cdecl*)()>(Addr(Ida::CurrentScreen))(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { screen = 0; }
        if (!screen) return;
        static const char kName[] = "PowerUp";
        uint32_t nameArg = reinterpret_cast<uint32_t>(kName);
        if (!TryCallAny1(Addr32(Ida::HashName), nameArg, nameArg, hash) || !hash) return;
        if (!TryCallAny1(Addr32(Ida::GadgetLookup), screen + 40, hash, gadget) || !gadget) return;
        __try { reinterpret_cast<int(__thiscall*)(void*)>(Addr(Ida::GadgetRefreshPage))(reinterpret_cast<void*>(gadget)); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void ApplyEventSettings() {
        Settings cur = ReadSettings();
        if (!cur.ok) return;
        g_saved = cur;
        g_settingsApplied = true;
        g_pmTouchedCount = 0;
        if (Features::On("eventsettings"))
            for (int i = 0; i < 4; ++i)
                if (cur.slots[i] != g_cfgSlots[i]) WriteU32(cur.obj + i * 4, g_cfgSlots[i]);
        if (cur.pm && cur.page != g_cfgPage) SetPowerupMgrPage(cur.pm, cur.page, g_cfgPage);
        LOG("[settings] applied (freeroam was %08X %08X %08X %08X page %d)", cur.slots[0], cur.slots[1],
            cur.slots[2], cur.slots[3], static_cast<int32_t>(cur.page));
    }

    static void RestoreEventSettings() {
        if (!g_settingsApplied) return;
        Settings cur = ReadSettings();
        if (cur.ok && cur.obj == g_saved.obj)
            for (int i = 0; i < 4; ++i)
                if (cur.slots[i] != g_saved.slots[i]) WriteU32(cur.obj + i * 4, g_saved.slots[i]);
        for (int i = 0; i < g_pmTouchedCount; ++i) WriteU32(g_pmTouched[i] + kPowerupPageOffset, g_pmOrigPage[i]);
        g_pmTouchedCount = 0;
        g_settingsApplied = false;
        SetPowerupPageNative(g_freeroamPage);
        RefreshPowerupGadget();
        g_restoreRefreshStep = 0;
        g_restoreRefreshDueMs = GetTickCount64() + 500;
        LOG("[settings] restored, powerup page %u", g_freeroamPage);
    }

    static void WatchEventSettings(ULONGLONG now) {
        Settings s = ReadSettings();
        if (!s.ok) return;
        if (!g_active) {
            if (FsmIndex() == kFsmFreeroam && !g_settingsApplied && g_restoreRefreshDueMs == 0 && s.page != 0xFFFFFFFF)
                g_freeroamPage = s.page;
            if (g_restoreRefreshDueMs != 0 && now >= g_restoreRefreshDueMs) {
                if (s.pm && s.page != g_freeroamPage) WriteU32(s.pm + kPowerupPageOffset, g_freeroamPage);
                RefreshPowerupGadget();
                g_restoreRefreshDueMs = (++g_restoreRefreshStep < 2) ? now + 1000 : 0;
            }
        }
        else if (g_settingsApplied && s.pm && s.page != g_cfgPage) {
            SetPowerupMgrPage(s.pm, s.page, g_cfgPage);
        }
    }

    struct CopMgr { uint32_t icop = 0, pursuit = 0, candidates = 0, managed = 0; bool ok = false; };

    static CopMgr ReadCopMgr() {
        CopMgr c;
        if (!ReadU32(Addr(Ida::CopMgrPtr), c.icop) || !c.icop) return c;
        c.ok = ReadU32(c.icop + kCopMgrPursuit, c.pursuit) && ReadU32(c.icop + kCandidateCount, c.candidates) &&
               ReadU32(c.icop + kManagedCount, c.managed);
        return c;
    }

    static uint32_t CopyList(uint32_t listBase, uint32_t count, uint32_t* out) {
        uint32_t n = count > kMaxCops ? kMaxCops : count;
        for (uint32_t i = 0; i < n; ++i) if (!ReadU32(listBase + i * 4, out[i])) out[i] = 0;
        return n;
    }

    static void AdoptCandidates(const CopMgr& c) {
        uint32_t list = 0, vt = 0, adoptFn = 0, cops[kMaxCops] = {};
        if (!c.candidates || !ReadU32(c.icop + kCandidates, list) || !list) return;
        if (!ReadU32(c.icop, vt) || !ReadU32(vt + kAdoptVt, adoptFn) || !adoptFn) return;
        uint32_t n = CopyList(list, c.candidates, cops);
        for (uint32_t i = 0; i < n; ++i) {
            if (!cops[i]) continue;
            __try {
                uint32_t id = CallAny1(Addr32(Ida::CopId), cops[i], cops[i]);
                reinterpret_cast<int(__thiscall*)(void*, int, int)>(adoptFn)(
                    reinterpret_cast<void*>(c.icop), static_cast<int>(cops[i]), static_cast<int>(id));
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        }
    }

    static void AttachManagedCops(const CopMgr& c) {
        uint32_t list = 0, iface = c.pursuit + 36, ifaceVt = 0, addFn = 0, cops[kMaxCops] = {};
        if (!c.managed || !ReadU32(c.icop + kManaged, list) || !list) return;
        if (!ReadU32(iface, ifaceVt) || !ReadU32(ifaceVt + 8, addFn) || !addFn) return;
        uint32_t n = CopyList(list, c.managed, cops);
        for (uint32_t i = 0; i < n; ++i) {
            if (!cops[i]) continue;
            __try { reinterpret_cast<uint8_t(__thiscall*)(void*, int)>(addFn)(reinterpret_cast<void*>(iface), static_cast<int>(cops[i])); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        }
        LOG("[cops] %u managed cop(s) attached to AIPursuit 0x%08X", n, c.pursuit);
    }

    static uint32_t PlayerSimable() {
        uint32_t holder = 0, player = 0, vt = 0, fn = 0;
        if (!ReadU32(Addr(Ida::LocalPlayerHolder), holder) || !holder || !ReadU32(holder, player) || !player) return 0;
        if (!ReadU32(player, vt) || !ReadU32(vt + 4, fn) || !fn) return 0;
        __try { return CallAny1(fn, player, player); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    static float PlayerHeat() {
        if (!Features::On("heatread")) return -1.0f;
        uint32_t simable = PlayerSimable(), table = 0, vt = 0, fn = 0;
        if (!simable || !ReadU32(simable + 4, table) || !table) return -1.0f;
        void* perp = nullptr;
        __try {
            perp = reinterpret_cast<void*(__thiscall*)(void*, void*)>(Addr(Ida::QueryInterface))(
                reinterpret_cast<void*>(static_cast<uintptr_t>(table)), reinterpret_cast<void*>(Addr(Ida::KeyIPerpetrator)));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1.0f; }
        if (!perp || !ReadU32(reinterpret_cast<uintptr_t>(perp), vt) || !ReadU32(vt + 4, fn) || !fn) return -1.0f;
        __try { return reinterpret_cast<float(__thiscall*)(void*)>(fn)(perp); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return -1.0f; }
    }

    static bool PlayerSetHeat(float heat) {
        uint32_t simable = PlayerSimable(), table = 0, vt = 0, fn = 0;
        if (!simable || !ReadU32(simable + 4, table) || !table) return false;
        void* perp = nullptr;
        __try {
            perp = reinterpret_cast<void*(__thiscall*)(void*, void*)>(Addr(Ida::QueryInterface))(
                reinterpret_cast<void*>(static_cast<uintptr_t>(table)), reinterpret_cast<void*>(Addr(Ida::KeyIPerpetrator)));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        if (!perp || !ReadU32(reinterpret_cast<uintptr_t>(perp), vt) || !ReadU32(vt + 12, fn) || !fn) return false;
        __try { reinterpret_cast<void(__thiscall*)(void*, float)>(fn)(perp, heat); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return true;
    }

    static void GiveCopGoal(uint32_t cop, uint32_t simable) {
        uint32_t table = 0, ai = 0, vt = 0;
        if (!ReadU32(cop + 4, table) || !table) return;
        __try {
            ai = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(reinterpret_cast<void*(__thiscall*)(void*, void*)>(
                Addr(Ida::QueryInterface))(reinterpret_cast<void*>(static_cast<uintptr_t>(table)),
                                           reinterpret_cast<void*>(Addr(Ida::KeyIPursuitAI)))));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        if (!ai || !ReadU32(ai, vt) || !vt) return;
        uint32_t target = Addr32(Ida::StartPursuitAI), p = 0;
        bool found = false;
        for (int i = 0; i < 48 && ReadU32(vt + i * 4, p); ++i) if (p == target) { found = true; break; }
        if (!found) return;
        __try {
            reinterpret_cast<int(__thiscall*)(void*, int, int)>(target)(
                reinterpret_cast<void*>(static_cast<uintptr_t>(ai)), 0, static_cast<int>(simable));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    constexpr uint32_t kRoadblockAddVehicle = 0x7E6EB0;
    constexpr int kMaxRoadblockCops = 64;
    static uint32_t g_rbVehicle[kMaxRoadblockCops] = {};
    static volatile LONG g_rbNext = 0;
    using RoadblockAdd_t = char(__thiscall*)(void*, int);
    static RoadblockAdd_t g_origRoadblockAdd = nullptr;

    static char __fastcall RoadblockAddHook(void* roadblock, void* /*edx*/, int vehicle) {
        const LONG i = (InterlockedIncrement(&g_rbNext) - 1) % kMaxRoadblockCops;
        g_rbVehicle[i] = static_cast<uint32_t>(vehicle);
        LOG("[cops] roadblock 0x%08X takes vehicle 0x%08X", static_cast<uint32_t>(reinterpret_cast<uintptr_t>(roadblock)), g_rbVehicle[i]);
        return g_origRoadblockAdd(roadblock, vehicle);
    }

    static bool IsRoadblockCop(uint32_t cop) {
        if (!cop || !Features::On("roadblockai")) return false;
        for (int i = 0; i < kMaxRoadblockCops; ++i) if (g_rbVehicle[i] == cop) return true;
        return false;
    }

    static uint32_t g_goalCop[kMaxTracked] = {};
    static ULONGLONG g_goalAtMs[kMaxTracked] = {};
    static bool g_goalFollowup[kMaxTracked] = {};
    static int g_goalCount = 0;

    static void GiveGoals(const CopMgr& c, ULONGLONG now) {
        uint32_t list = 0, cops[kMaxCops] = {};
        if (!c.pursuit || !c.managed || !ReadU32(c.icop + kManaged, list) || !list) return;
        uint32_t simable = PlayerSimable();
        if (!simable) return;
        uint32_t n = CopyList(list, c.managed, cops);
        int kept = 0;
        for (int j = 0; j < g_goalCount; ++j) {
            bool present = false;
            for (uint32_t i = 0; i < n; ++i) if (cops[i] == g_goalCop[j]) present = true;
            if (!present) continue;
            g_goalCop[kept] = g_goalCop[j]; g_goalAtMs[kept] = g_goalAtMs[j]; g_goalFollowup[kept] = g_goalFollowup[j];
            ++kept;
        }
        g_goalCount = kept;
        for (int j = 0; j < g_goalCount; ++j) {
            if (!g_goalFollowup[j] && now - g_goalAtMs[j] >= kGoalFollowupMs) {
                g_goalFollowup[j] = true;
                if (!IsRoadblockCop(g_goalCop[j])) GiveCopGoal(g_goalCop[j], simable);
            }
        }
        for (uint32_t i = 0; i < n; ++i) {
            bool known = !cops[i];
            for (int j = 0; j < g_goalCount && !known; ++j) known = g_goalCop[j] == cops[i];
            if (known) continue;
            if (!IsRoadblockCop(cops[i])) GiveCopGoal(cops[i], simable);
            if (g_goalCount < kMaxTracked) {
                g_goalCop[g_goalCount] = cops[i]; g_goalAtMs[g_goalCount] = now; g_goalFollowup[g_goalCount] = false;
                ++g_goalCount;
            }
        }
    }

    static void SetActiveFlags() { g_resultsWindowFlag.store(g_active || g_resultsPhase, std::memory_order_release); }

    static void StartGameplay(ULONGLONG now) {
        CopMgr pre = ReadCopMgr();
        if (pre.ok && pre.pursuit) {
            LOG("[pursuit] a pursuit is already running (AIPursuit 0x%08X) -- ours not started", pre.pursuit);
            Music::Restore();
            return;
        }
        float carHeat = PlayerHeat();
        int32_t heat = carHeat < kMinHeat ? kMinHeat : carHeat > kMaxHeat ? kMaxHeat : static_cast<int32_t>(carHeat);
        LOG("[pursuit] ---- step 1: LaunchPursuit(heat %d, car heat %.2f) ----", heat, carHeat);
        g_goalCount = 0;
        g_statStartMs = now; g_statEndMs = 0; g_statMaxCops = 0; g_statBusted = false; g_statHeat = 0.0f;
        ::Mod::Stats::Reset();
        Results::PursuitStarted();
        SpotFx::PursuitStarted();
        ApplyEventSettings();
        g_modeOverride.store(true);
        SetPowerupPageNative(g_cfgPage);
        if (Features::On("music")) Music::ApplyPursuit(g_cfgSlots[0]);
        if (Features::On("mapicons")) MapIcons::EnterPursuit();
        if (Features::On("markers")) MarkerFx::Hide();
        Guard::NoteLaunch(GetTickCount64());
        bool ok = LaunchPursuit(heat);
        LOG("[pursuit] LaunchPursuit %s", ok ? "called" : "FAILED");
        if (ok && carHeat > static_cast<float>(heat) && Features::On("heatread")) {
            float h = carHeat > kMaxHeat ? static_cast<float>(kMaxHeat) : carHeat;
            bool set = PlayerSetHeat(h);
            LOG("[pursuit] car heat %.2f restored after LaunchPursuit (%s, now %.2f)", h, set ? "ok" : "FAILED", PlayerHeat());
            (void)set;
        }
        CopMgr c = ReadCopMgr();
        if (c.ok && c.pursuit) {
            AdoptCandidates(c);
            c = ReadCopMgr();
            AttachManagedCops(c);
            GiveGoals(c, now);
        }
        g_active = true;
        g_uiEntered = false;
        g_cooldown = false;
        g_sawAIPursuit = false;
        g_aiPursuitGoneMs = 0;
        SetActiveFlags();
        g_uiStepDueMs = now + kUiStepDelayMs;
    }

    static void EnterPursuitScreen(ULONGLONG now) {
        uint32_t guard = ReadU32Or(Addr(Ida::Fsm) + 0x24, 0);
        uint32_t before = FsmIndex();
        if (!guard || before != kFsmFreeroam || !CallSetFsm(kFsmPursuit)) {
            LOG("[pursuit] FSM flip refused/failed (index %u) -- gameplay-only pursuit", before);
            return;
        }
        LOG("[pursuit] ---- step 2: FSM %u -> %u ----", before, FsmIndex());
        g_uiEntered = true;
        ShowPursuitGadgets();
        if (g_settingsApplied) RefreshPowerupGadget();
        g_gadgetReplayDueMs = now + kGadgetReplayAgainMs;
    }

    static void ShowResults(ULONGLONG now) {
        g_statEndMs = now;
        g_exitCallsAtResults = Hooks::ExitPursuitModeCalls();
        char r = SetGadgetVisible("PostRace", 1);
        LOG("[pursuit] AIPursuit gone -> results screen (PostRace -> %d)", (int)r);
        (void)r;
        g_resultsPhase = true;
        g_resultsStartMs = now;
        SetActiveFlags();
    }

    static void EndPursuit(const char* why, bool callExit) {
        LOG("[pursuit] ---- end: %s ----", why);
        if (g_resultsPhase) Results::Commit();
        (void)why;
        if (callExit) {
            __try { reinterpret_cast<int(__thiscall*)(void*)>(Addr(Ida::ExitPursuitMode))(g_exitDummyThis); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        Music::Restore();
        SpotFx::Off();
        MapIcons::ExitPursuit();
        MarkerFx::Restore();
        g_resultsWindowUntilMs.store(GetTickCount64() + kResultsWindowAfterExit);
        g_modeOverride.store(false);
        if (FsmIndex() == kFsmPursuit) CallSetFsm(kFsmFreeroam);
        g_active = false;
        g_uiEntered = false;
        g_resultsPhase = false;
        g_sawAIPursuit = false;
        g_aiPursuitGoneMs = 0;
        g_gadgetReplayDueMs = 0;
        SetActiveFlags();
        RestoreEventSettings();
    }

    static void Watch(ULONGLONG now) {
        CopMgr c = ReadCopMgr();
        WatchEventSettings(now);
        ULONGLONG until = g_resultsWindowUntilMs.load(std::memory_order_relaxed);
        if (!g_active && until && now + kResultsWindowAfterExit > until + 2000 && FsmIndex() == kFsmPursuit) {
            g_resultsWindowUntilMs.store(0);
            LOG("[results] real event started -- our results window closed");
        }
        if (!g_active) return;
        if (g_resultsPhase) {
            if (::Mod::Log::Enabled()) {
                static ULONGLONG s_beatMs = 0;
                if (now - s_beatMs >= 5000) {
                    s_beatMs = now;
                    LOG("[results] waiting for the results screen to close: %llus, FSM=%u, exit calls %u, copmgr %s",
                        (now - g_resultsStartMs) / 1000, FsmIndex(), Hooks::ExitPursuitModeCalls(), c.ok ? "ok" : "GONE");
                }
            }
            if (Hooks::ExitPursuitModeCalls() != g_exitCallsAtResults) EndPursuit("results screen closed", false);
            else if (now - g_resultsStartMs >= kResultsTimeoutMs) EndPursuit("results timeout", true);
            return;
        }
        if (!c.ok) return;

        if (c.pursuit && c.managed > g_statMaxCops) g_statMaxCops = c.managed;
        if (!g_resultsPhase && !g_aiPursuitGoneMs) MarkerFx::Tick();
        if (g_gadgetReplayDueMs && now >= g_gadgetReplayDueMs) {
            g_gadgetReplayDueMs = 0;
            ShowPursuitGadgets();
            if (g_settingsApplied) RefreshPowerupGadget();
        }
        if (c.pursuit) {
            g_sawAIPursuit = true;
            g_aiPursuitGoneMs = 0;
            static ULONGLONG s_nextAdopt = 0;
            if (!g_cooldown && now >= s_nextAdopt && Features::On("roadblocks")) {
                s_nextAdopt = now + 1000;
                if (c.candidates) {
                    uint32_t before = c.managed, cand = c.candidates;
                    AdoptCandidates(c);
                    c = ReadCopMgr();
                    if (c.ok && c.pursuit && c.managed != before) {
                        LOG("[cops] %u new candidate(s) adopted (managed %u -> %u)", cand, before, c.managed);
                        AttachManagedCops(c);
                    }
                }
            }
            if (!c.ok || !c.pursuit) return;
            GiveGoals(c, now);
            uint32_t cd = 0;
            if (!g_resultsPhase && ReadU32(c.pursuit + kPursuitCooldown, cd)) {
                bool on = (cd & 0xFF) != 0;
                if (on != g_cooldown) {
                    g_cooldown = on;
                    if (Features::On("mapicons")) MapIcons::SetCooldown(on);
                    SpotFx::SetCooldown(on);
                }
            }
        }
        else if (g_sawAIPursuit && !g_resultsPhase) {
            if (!g_aiPursuitGoneMs) {
                g_statBusted = !g_cooldown;
                g_statHeat = PlayerHeat();
                LOG("[pursuit] chase over: %s (heat %.2f)", g_statBusted ? "BUSTED" : "EVADED", g_statHeat);
                
                if (g_statBusted) {
                     PlayerSetHeat(kMinHeat);
                }
                
                Results::ChaseOver();
                if (Features::On("mapicons")) MapIcons::HidePursuitIcons();
                SpotFx::Off();
            }
            if (!g_aiPursuitGoneMs) g_aiPursuitGoneMs = now;
            else if (now - g_aiPursuitGoneMs >= kEndAfterPursuitGoneMs) {
                if (g_uiEntered) ShowResults(now);
                else EndPursuit("AIPursuit gone (gameplay-only pursuit)", false);
            }
        }
    }

    void PumpGameThread() {
        if (!OnGameThread()) return;
        ULONGLONG now = GetTickCount64();
        Music::Tick(now);
        Results::Tick(now);

        if (now >= g_nextWatchMs) {
            g_nextWatchMs = now + kWatchPeriodMs;
            Watch(now);
        }

        if (g_hitRequested.exchange(false, std::memory_order_acq_rel) && !g_active && !g_step1DueMs &&
            FsmIndex() == kFsmFreeroam) {
            LOG("[pursuit] ---- cop hit ----");
            Music::BeginPulse(now);
            g_step1DueMs = now + kStep1DelayMs;
            return;
        }
        if (g_step1DueMs && now >= g_step1DueMs) {
            g_step1DueMs = 0;
            StartGameplay(now);
            return;
        }
        if (g_uiStepDueMs && now >= g_uiStepDueMs) {
            g_uiStepDueMs = 0;
            if (g_active) EnterPursuitScreen(now);
        }
    }

    void RequestHit() { g_hitRequested.store(true, std::memory_order_release); }

    bool IsActive() { return g_resultsWindowFlag.load(std::memory_order_acquire); }

    bool InResultsWindow() {
        return g_resultsWindowFlag.load(std::memory_order_acquire) ||
               GetTickCount64() < g_resultsWindowUntilMs.load(std::memory_order_relaxed);
    }

    Stats GetStats() { return Stats{ g_statStartMs, g_statEndMs, g_statMaxCops, g_statBusted, g_statHeat }; }

    void Install() {
        Features::Load();
        LoadConfig();
        g_origModeGetter = reinterpret_cast<ModeGetter_t>(Addr(Ida::ModeGetter));
        Hooks::Attach(reinterpret_cast<void**>(&g_origModeGetter), reinterpret_cast<void*>(ModeGetterHook), "mode getter");
        g_origRoadblockAdd = reinterpret_cast<RoadblockAdd_t>(Addr(kRoadblockAddVehicle));
        Hooks::Attach(reinterpret_cast<void**>(&g_origRoadblockAdd), reinterpret_cast<void*>(RoadblockAddHook), "roadblock add vehicle");
    }

    void Remove() {
        g_modeOverride.store(false);
        Hooks::Detach(reinterpret_cast<void**>(&g_origModeGetter), reinterpret_cast<void*>(ModeGetterHook));
        if (g_origRoadblockAdd) Hooks::Detach(reinterpret_cast<void**>(&g_origRoadblockAdd), reinterpret_cast<void*>(RoadblockAddHook));
    }
}
