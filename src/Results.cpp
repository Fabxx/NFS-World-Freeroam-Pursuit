// Results screen: arbitrated packet, accolades (event 385 formula), lucky draw, rewards saved on the server.
#include "Results.h"
#include "Game.h"
#include "Hooks.h"
#include "Pursuit.h"
#include "Log.h"
#include "Features.h"
#include "Stats.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

namespace Mod::Results {
    constexpr int kEntrantWords = 270;
    constexpr int kPursuitBlockWord = 260;
    namespace E {
        constexpr uint32_t Rank = 188, FinishReason = 192, Duration = 196, TimeDiff = 200, HasArbitrated = 220,
            NumPowerups = 224, CopsDeployed = 1040, CopsDisabled = 1044, CopsRammed = 1048, CostToState = 1052,
            Heat = 1056, Infractions = 1060, LongestJump = 1064, RoadBlocks = 1068, SpikeStrips = 1072, TopSpeed = 1076;
    }
    constexpr uint32_t kPacketEntrant = 64, kPacketHasArbitrated = 60;
    constexpr uint32_t kFinishEvaded = 0x206;
    constexpr uint32_t kFinishBusted = 0x10A;
    constexpr uint32_t kCostPerCop = 250;

    static uint32_t g_words[kEntrantWords] = {};
    static uint8_t g_kind[kEntrantWords] = {};
    static char g_str[kEntrantWords][64] = {};
    static bool g_valid = false;

    static std::string TemplatePath() { return ModuleDir() + "NFSWorldPursuitProbe_entrant.txt"; }

    static void SaveTemplate() {
        FILE* f = nullptr;
        if (fopen_s(&f, TemplatePath().c_str(), "w") != 0 || !f) return;
        for (int i = 1; i < kEntrantWords; ++i) {
            if (g_kind[i] == 1) fprintf(f, "V %03X %08X\n", i * 4, g_words[i]);
            else if (g_kind[i] == 2 && g_str[i][0]) fprintf(f, "S %03X %s\n", i * 4, g_str[i]);
        }
        fclose(f);
    }

    struct DefValue { uint16_t off; uint32_t v; };
    struct DefString { uint16_t off; const char* s; };
    static const DefValue kDefValues[] = {
        { 0x018, 0x00000064 }, { 0x020, 0x00000017 }, { 0x028, 0x000019E1 }, { 0x02C, 0x00000028 },
        { 0x030, 0x00000001 }, { 0x060, 0x9B20A618 }, { 0x064, 0x00000001 }, { 0x0BC, 0x00000001 },
        { 0x0C0, 0x00000206 }, { 0x0DC, 0x00000001 }, { 0x0E0, 0x00000001 }, { 0x0FC, 0x4E085FB3 },
        { 0x120, 0x3F8CCC92 }, { 0x194, 0x00000001 },
    };
    static const DefString kDefStrings[] = {
        { 0x034, "Fabx" }, { 0x050, "product_cooldown_x1" }, { 0x0CC, "Beta is here" }, { 0x100, "BMW" },
        { 0x110, "M3 GTR E46 (RACE SPEC)" }, { 0x124, "M3 GTR E46 (RACE SPEC)" }, { 0x14C, "LBL_EMPTY_PACKAGE" },
        { 0x1A8, "BMW M3 GTR E46 (RACE SPEC)" }, { 0x1D8, "800" }, { 0x220, "820" }, { 0x268, "760" }, { 0x2A4, "793" },
    };

    static void LoadDefaultTemplate() {
        for (const DefValue& d : kDefValues) { g_words[d.off / 4] = d.v; g_kind[d.off / 4] = 1; }
        for (const DefString& d : kDefStrings) { strcpy_s(g_str[d.off / 4], d.s); g_kind[d.off / 4] = 2; }
        g_valid = true;
        LOG("[results] entrant template: built-in default");
    }

    static void LoadTemplate() {
        if (!Features::On("template")) { LoadDefaultTemplate(); return; }
        FILE* f = nullptr;
        if (fopen_s(&f, TemplatePath().c_str(), "r") != 0 || !f) { LoadDefaultTemplate(); return; }
        char line[256];
        int n = 0;
        while (fgets(line, sizeof(line), f)) {
            unsigned off = 0, v = 0;
            size_t len = strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
            if (line[0] == 'S' && line[1] == ' ' && sscanf_s(line + 2, "%X", &off) == 1 && off < 1080 && !(off & 3)) {
                const char* text = strchr(line + 2, ' ');
                if (text) { strncpy_s(g_str[off / 4], text + 1, 63); g_kind[off / 4] = 2; ++n; }
            }
            else if (line[0] == 'V' && sscanf_s(line + 2, "%X %X", &off, &v) == 2 && off < 1080 && !(off & 3)) {
                g_words[off / 4] = v; g_kind[off / 4] = 1; ++n;
            }
        }
        fclose(f);
        LOG("[results] entrant template: %d field(s) from %s", n, TemplatePath().c_str());
        if (n > 0 && g_words[E::FinishReason / 4] != kFinishEvaded) {
            LOG("[results] template comes from a non-evaded result (finish %08X) -- built-in default used",
                g_words[E::FinishReason / 4]);
            memset(g_words, 0, sizeof(g_words)); memset(g_kind, 0, sizeof(g_kind)); memset(g_str, 0, sizeof(g_str));
            n = 0;
        }
        if (n > 0) g_valid = true;
        else { LoadDefaultTemplate(); SaveTemplate(); }
    }

    static bool Readable(uint32_t a) {
        if (a < 0x10000) return false;
        __try { volatile uint8_t b = *reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(a)); (void)b; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool CopyBytes(void* dst, uint32_t src, uint32_t n) {
        __try { memcpy(dst, reinterpret_cast<const void*>(static_cast<uintptr_t>(src)), n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool Capture(uint32_t base, int n, uint32_t* w, uint8_t* kind, char (*str)[64]) {
        if (base < 0x10000 || !CopyBytes(w, base, n * 4)) return false;
        memset(kind, 0, n);
        memset(str, 0, n * 64);
        for (int i = 1; i < n; ++i) {
            uint32_t v = w[i];
            if (!v) continue;
            if (!Readable(v)) { kind[i] = 1; continue; }
            if (i + 2 < n && w[i] <= w[i + 1] && w[i + 1] <= w[i + 2] && w[i + 1] - w[i] < 63 && w[i + 2] - w[i] < 4096) {
                uint32_t len = w[i + 1] - w[i];
                if (!CopyBytes(str[i], v, len)) continue;
                str[i][len] = 0;
                kind[i] = 2;
                i += 2;
            }
        }
        return true;
    }

    static void __stdcall OnEntrantSerialize(uint32_t entrant) {
        if (Pursuit::InResultsWindow()) return;
        if (!Features::On("template")) return;
        static uint32_t w[kEntrantWords];
        static uint8_t kind[kEntrantWords];
        static char str[kEntrantWords][64];
        if (!Capture(entrant, kEntrantWords, w, kind, str)) return;
        if (w[E::FinishReason / 4] != kFinishEvaded) {
            LOG("[results] real result not evaded (finish %08X) -- template not captured", w[E::FinishReason / 4]);
            return;
        }
        bool same = g_valid && memcmp(kind, g_kind, sizeof(g_kind)) == 0 && memcmp(str, g_str, sizeof(g_str)) == 0;
        for (int i = 0; i < kEntrantWords && same; ++i) if (kind[i] == 1 && w[i] != g_words[i]) same = false;
        if (same) return;
        memcpy(g_words, w, sizeof(w));
        memcpy(g_kind, kind, sizeof(kind));
        memcpy(g_str, str, sizeof(str));
        g_valid = true;
        SaveTemplate();
        LOG("[results] entrant template captured from a real results screen");
    }

    static uint32_t g_origEntrantSerialize = 0;

    static __declspec(naked) void EntrantSerializeStub() {
        __asm {
            pushfd
            pushad
            push ecx
            call OnEntrantSerialize
            popad
            popfd
            jmp  dword ptr [g_origEntrantSerialize]
        }
    }

    constexpr uint32_t kCarDataWord = 0xE8 / 4;
    constexpr int kCarDataWords = kPursuitBlockWord - kCarDataWord;

    static void __stdcall OnCarDataSerialize(uint32_t carData, uint32_t ret) {
        uint32_t site = static_cast<uint32_t>(ret - ExeBase() + 0x400000);
        if (site != 0x494952 && site != 0x51B138) return;
        if (!Features::On("carcapture") || !Features::On("template")) return;
        static uint32_t w[kCarDataWords];
        static uint8_t kind[kCarDataWords];
        static char str[kCarDataWords][64];
        if (!Capture(carData, kCarDataWords, w, kind, str)) return;
        bool same = true;
        for (int i = 1; i < kCarDataWords && same; ++i) {
            int t = kCarDataWord + i;
            if (kind[i] != g_kind[t] || (kind[i] == 1 && w[i] != g_words[t]) || (kind[i] == 2 && strcmp(str[i], g_str[t]) != 0))
                same = false;
        }
        if (same) return;
        for (int i = 1; i < kCarDataWords; ++i) {
            int t = kCarDataWord + i;
            g_kind[t] = kind[i];
            g_words[t] = w[i];
            memcpy(g_str[t], str[i], sizeof(g_str[t]));
        }
        g_valid = true;
        SaveTemplate();
        LOG("[results] current car updated: \"%s\" (from 0x%06X)", g_str[kCarDataWord + 0xC0 / 4], site);
    }

    static uint32_t g_origCarDataSerialize = 0;

    static __declspec(naked) void CarDataSerializeStub() {
        __asm {
            pushfd
            pushad
            push dword ptr [esp + 36]
            push ecx
            call OnCarDataSerialize
            popad
            popfd
            jmp  dword ptr [g_origCarDataSerialize]
        }
    }

    static uint32_t BuildRequest(int* args) {
        uint32_t v1 = 0, sink = 0, argc = 0, argv = 0, flags = 0, name = 0;
        if (!args || !ReadU32(reinterpret_cast<uintptr_t>(args), v1) || !v1 || !ReadU32(v1 + 8, sink)) return 0;
        ReadU32(reinterpret_cast<uintptr_t>(args) + 12, argc);
        ReadU32(reinterpret_cast<uintptr_t>(args) + 8, argv);
        if (argc && argv) {
            ReadU32(argv + 4, flags);
            ReadU32(argv + 8, name);
            if (flags & 0x40) name = ReadU32Or(name, 0);
        }
        uint32_t* req = static_cast<uint32_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 64));
        if (!req) return 0;
        __try {
            if (argc && name) {
                reinterpret_cast<void*(__thiscall*)(void*, int, void*)>(Addr(Ida::ArbRequestCtor))(
                    req, static_cast<int>(sink), reinterpret_cast<void*>(static_cast<uintptr_t>(name)));
            } else {
                const uint32_t empty = Addr32(Ida::EmptyString);
                req[0] = Addr32(Ida::ArbRequestVtable);
                req[1] = sink;
                req[2] = empty; req[3] = empty; req[4] = empty + 1;
                req[6] = empty; req[7] = empty; req[8] = empty + 1;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        return reinterpret_cast<uint32_t>(req);
    }

    static void AssignString(uint32_t strObj, const char* text) {
        reinterpret_cast<void*(__thiscall*)(void*, const char*, const char*)>(Addr(Ida::StringAssign))(
            reinterpret_cast<void*>(static_cast<uintptr_t>(strObj)), text, text + strlen(text));
    }

    static volatile float* F32(uint32_t base, uint32_t off) {
        return reinterpret_cast<volatile float*>(static_cast<uintptr_t>(base + off));
    }

    struct PowerupInfo { int hash; const char* icon; };
    static const PowerupInfo kPowerups[] = {
        { -537557654, "product_run_flats_x1" }, { 125509666, "product_magnet_x1" },
        { -1692359144, "product_cooldown_x1" }, { -364944936, "product_shield_x1" },
        { 2236629, "product_slingshot_x1" }, { 957701799, "product_readiness_x1" },
        { 1805681994, "product_juggernaut_x1" }, { -611661916, "product_evade_x1" },
        { -1564932069, "product_evade_multi_x1" }, { -1681514783, "product_nos_x1" },
        { 1627606782, "product_more_lap_x1" }, { 1113720384, "product_slingshot_multi_x1" },
    };
    constexpr uint32_t kPowerupRecords = 76, kPowerupRecordSize = 28, kMaxPowerupRecords = 4;
    struct UsedPowerup { int hash, count; };
    static UsedPowerup g_used[16] = {};
    static volatile LONG g_usedCount = 0;
    static CRITICAL_SECTION g_usedLock;

    static const char* PowerupIcon(int hash) {
        for (const PowerupInfo& p : kPowerups) if (p.hash == hash) return p.icon;
        return nullptr;
    }

    static int FillPowerups(uint32_t ent, volatile uint32_t* e) {
        UsedPowerup used[16] = {};
        int n = 0;
        EnterCriticalSection(&g_usedLock);
        n = static_cast<int>(g_usedCount);
        memcpy(used, g_used, sizeof(used));
        LeaveCriticalSection(&g_usedLock);
        int written = 0, total = 0;
        for (int i = 0; i < n && written < static_cast<int>(kMaxPowerupRecords); ++i) {
            const char* icon = PowerupIcon(used[i].hash);
            total += used[i].count;
            if (!icon || used[i].count <= 0) continue;
            const uint32_t rec = ent + kPowerupRecords + kPowerupRecordSize * written;
            uint32_t vt = 0;
            const uint32_t mb = static_cast<uint32_t>(ExeBase());
            if (!ReadU32(rec, vt) || vt < mb || vt >= mb + 0x900000) {
                LOG("[results] powerup record %d has no vtable (0x%08X) -- powerups not written", written, vt);
                break;
            }
            AssignString(rec + 4, icon);
            e[(kPowerupRecords + kPowerupRecordSize * written + 20) / 4] = static_cast<uint32_t>(used[i].hash);
            e[(kPowerupRecords + kPowerupRecordSize * written + 24) / 4] = static_cast<uint32_t>(used[i].count);
            ++written;
        }
        if (n) LOG("[results] powerups used: %d (%d kind(s), %d record(s) written)", total, n, written);
        (void)total;
        return written;
    }

    static bool FillPacket(uint32_t packet) {
        const uint32_t ent = packet + kPacketEntrant;
        const uint32_t empty = Addr32(Ida::EmptyString), vtRating = Addr32(Ida::RatingVtable);
        Pursuit::Stats st = Pursuit::GetStats();
        ULONGLONG endMs = st.endMs ? st.endMs : GetTickCount64();
        float duration = st.startMs ? static_cast<float>((endMs - st.startMs) / 1000.0) : 0.0f;
        uint32_t cops = st.maxCops ? st.maxCops : 1;
        __try {
            volatile uint32_t* e = reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(ent));
            if (g_valid) {
                for (int i = 1; i < kPursuitBlockWord; ++i) {
                    if (g_kind[i] == 1 && e[i] == 0) e[i] = g_words[i];
                    else if (g_kind[i] == 2 && g_str[i][0] && e[i] == empty && e[i + 1] == empty) {
                        AssignString(ent + 4 * i, g_str[i]);
                        i += 2;
                    }
                }
                for (int i = 3; i < kPursuitBlockWord; ++i) {
                    const char* t = g_str[i];
                    if (g_kind[i] != 2 || !t[0] || e[i - 3] != vtRating) continue;
                    bool digits = true;
                    for (const char* c = t; *c; ++c) if (*c < '0' || *c > '9') { digits = false; break; }
                    if (!digits) continue;
                    int n = atoi(t);
                    *F32(ent, 4 * (i - 2)) = n / 1000.0f;
                    e[i - 1] = static_cast<uint32_t>(n);
                }
            }
            *reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(packet + kPacketHasArbitrated)) = 1;
            e[E::Rank / 4] = 1;
            e[E::FinishReason / 4] = (st.busted && Features::On("busted")) ? kFinishBusted : kFinishEvaded;
            *F32(ent, E::Duration) = duration;
            *F32(ent, E::TimeDiff) = 0.0f;
            *reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(ent + E::HasArbitrated)) = 1;
            e[E::NumPowerups / 4] = Features::On("powerupsused") ? static_cast<uint32_t>(FillPowerups(ent, e)) : 0;
            ::Mod::Stats::Snapshot hs = ::Mod::Stats::Get();
            e[E::CopsDeployed / 4] = hs.valid && hs.copsDeployed ? hs.copsDeployed : cops;
            e[E::CopsDisabled / 4] = hs.copsDisabled;
            e[E::CopsRammed / 4] = hs.copsRammed;
            e[E::CostToState / 4] = hs.valid && hs.costToState ? hs.costToState : cops * kCostPerCop;
            *F32(ent, E::Heat) = 1.0f;
            e[E::Infractions / 4] = 0;
            *F32(ent, E::LongestJump) = 0.0f;
            e[E::RoadBlocks / 4] = Features::On("roadblockhits") ? Pursuit::RoadblocksDodged() : 0;
            e[E::SpikeStrips / 4] = 0;
            *F32(ent, E::TopSpeed) = 0.0f;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        LOG("[results] packet 0x%08X: %s, duration %.1fs, %u cop(s), heat %.2f, template %s", packet, st.busted ? "BUSTED" : "EVADED",
            duration, cops, st.heat, g_valid ? "yes" : "no");
        return true;
    }

    static void Answer(uint32_t req) {
        uint32_t packet = 0;
        __try { packet = reinterpret_cast<uint32_t(__cdecl*)()>(Addr(Ida::NewPursuitPacket))(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { packet = 0; }
        if (!packet || !FillPacket(packet)) { LOG("[results] packet 0x%08X could not be built", packet); return; }
        bool ok = true;
        __try {
            reinterpret_cast<int(__thiscall*)(void*, uint32_t)>(Addr(Ida::ArbAnswer))(
                reinterpret_cast<void*>(static_cast<uintptr_t>(req)), packet);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        LOG("[results] answer (sub_51B0E0) %s", ok ? "done" : "FAULTED");
        (void)ok;
    }

    constexpr uint32_t kUiStateNative = 0x69E280;
    constexpr int32_t kUiArbitrationCompleted = 12;
    constexpr ULONGLONG kArbCompletedDelayMs = 500;
    constexpr uint32_t kAccoladesHandler = 0x51E7B0, kNewAccolades = 0x699C50, kGameAlloc = 0x61C2A0,
                       kGameAllocPool = 0xC81794, kStringFromC = 0x403770;
    static ULONGLONG g_arbCompletedDueMs = 0;

    static volatile LONG g_playerLevel = 0;
    static uint32_t g_origPersonaSer = 0;
    static void __stdcall OnPersonaSerialize(uint32_t persona) {
        uint32_t lvl = 0, w = 0;
        if (Pursuit::InResultsWindow()) return;
        if (!ReadU32(persona + 44, lvl) || !ReadU32(persona + 48, w) || !(w & 0xFF) || lvl == 0 || lvl > 100) return;
        if (InterlockedExchange(&g_playerLevel, static_cast<LONG>(lvl)) != static_cast<LONG>(lvl))
            LOG("[results] persona level %u", lvl);
    }
    static __declspec(naked) void PersonaSerStub() {
        __asm {
            pushfd
            pushad
            push ecx
            call OnPersonaSerialize
            popad
            popfd
            jmp  dword ptr [g_origPersonaSer]
        }
    }
    static int PlayerLevel() { LONG l = g_playerLevel; return l > 0 ? static_cast<int>(l) : 1; }
    struct Part { const char* type; const char* cat; int rep, cash; };
    struct Rewards { int cash, rep, nParts; Part parts[16]; };
    static Rewards ComputeRewards(const Pursuit::Stats& st, uint32_t cops, float durationS) {
        Rewards r = {};
        (void)durationS;
        if (st.busted) return r;
        const double baseCash = 60.0 * PlayerLevel(), baseRep = 35.0 * PlayerLevel();
        auto add = [&r](const char* type, const char* cat, int rep, int cash) {
            if ((rep || cash) && r.nParts < 16) { r.parts[r.nParts++] = Part{ type, cat, rep, cash }; r.rep += rep; r.cash += cash; }
        };
        add("None", "Base", static_cast<int>(baseRep), static_cast<int>(baseCash));
        add("None", "Rank", static_cast<int>(baseRep), static_cast<int>(baseCash));
        ::Mod::Stats::Snapshot hs = ::Mod::Stats::Get();
        const double deployed = hs.valid && hs.copsDeployed ? hs.copsDeployed : cops;
        const double cost = hs.valid && hs.costToState ? hs.costToState : static_cast<double>(cops * kCostPerCop);
        struct M { const char* type; double stat, mul; } m[] = {
            { "CopCarsDeployed", deployed, 0.025 },
            { "CopCarsDisabled", static_cast<double>(hs.copsDisabled), 0.1 },
            { "CopCarsRammed", static_cast<double>(hs.copsRammed), 0.05 },
            { "CostToState", cost, 0.00005 },
            { "HeatLevel", static_cast<double>(static_cast<int>(st.heat > 0 ? st.heat : 1.0f)), 0.2 },
        };
        for (const M& x : m) add(x.type, "Pursuit", static_cast<int>(baseRep * x.stat * x.mul), static_cast<int>(baseCash * x.stat * x.mul));
        return r;
    }

    struct Lucky { const char* icon; const char* title; int hash; const char* tag; };
    static const Lucky kLucky[] = {
        { "product_run_flats_x1_no_bg", "RUN FLATS", -537557654, "runflattires" },
        { "product_cooldown_x1_no_bg", "INSTANT COOLDOWN", -1692359144, "instantcooldown" },
        { "product_slingshot_x1_no_bg", "SLINGSHOT", 2236629, "slingshot" },
        { "product_readiness_x1_no_bg", "READY!", 957701799, "ready" },
        { "product_juggernaut_x1_no_bg", "JUGGERNAUT", 1805681994, "juggernaut" },
        { "product_evade_x1_no_bg", "EMERGENCY EVADE", -611661916, "emergencyevade" },
        { "product_evade_multi_x1_no_bg", "TEAM EMERGENCY EVADE", -1564932069, "team_emergencyevade" },
        { "product_nos_x1_no_bg", "NITROUS", -1681514783, "nosshot" },
        { "product_slingshot_multi_x1_no_bg", "TEAM SLINGSHOT", 1113720384, "team_slingshot" },
    };
    constexpr uint32_t kNewRewardPart = 0x699AD0, kAddRewardPart = 0x6A9E00, kNewLuckyItem = 0x699B30, kAddLuckyItem = 0x6A9DA0;

    static void SetStr(uint32_t at, const char* text) {
        reinterpret_cast<void*(__thiscall*)(void*, const char*, const char*)>(Addr(Ida::StringAssign))(
            reinterpret_cast<void*>(static_cast<uintptr_t>(at)), text, text + strlen(text));
    }

    static void DeleteObj(uint32_t obj, int slot) {
        uint32_t vt = 0, fn = 0;
        if (!ReadU32(obj, vt) || !ReadU32(vt + 4 * slot, fn) || !fn) return;
        reinterpret_cast<void*(__thiscall*)(void*, int)>(fn)(reinterpret_cast<void*>(static_cast<uintptr_t>(obj)), 1);
    }

    static int g_luckyIndex = -1;
    static volatile LONG g_leveledUp = 0;

    static bool g_haveRewards = false;
    static Rewards g_rewards = {};
    static const Rewards& ScreenRewards(const Pursuit::Stats& st) {
        if (!g_haveRewards) {
            ULONGLONG endMs = st.endMs ? st.endMs : GetTickCount64();
            float duration = st.startMs ? static_cast<float>((endMs - st.startMs) / 1000.0) : 0.0f;
            g_rewards = ComputeRewards(st, st.maxCops ? st.maxCops : 1, duration);
            g_haveRewards = true;
            LOG("[results] rewards for this screen: %d cash, %d REP (level %d)", g_rewards.cash, g_rewards.rep, PlayerLevel());
        }
        return g_rewards;
    }

    static void FillAccolades(uint32_t acc, const Rewards& r, bool busted, char* luckyOut, int luckyCap) {
        volatile uint32_t* a = reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(acc));
        a[1] = static_cast<uint32_t>(r.rep);
        a[2] = static_cast<uint32_t>(r.cash);
        a[3] = static_cast<uint32_t>(r.rep);
        a[4] = static_cast<uint32_t>(r.cash);
        *reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(acc + 20)) = g_leveledUp ? 1 : 0;
        char name[64];
        for (int i = 0; i < r.nParts; ++i) {
            const Part& p = r.parts[i];
            for (int k = 0; k < 2; ++k) {
                uint32_t part = reinterpret_cast<uint32_t(__cdecl*)()>(Addr(kNewRewardPart))();
                if (!part) continue;
                snprintf(name, sizeof(name), "REWARD_TYPE_%s_%s", p.type, k == 0 ? "REP" : "TOKEN");
                SetStr(part + 4, name);
                SetStr(part + 20, k == 0 ? "rep" : "tokens");
                SetStr(part + 36, p.cat);
                *reinterpret_cast<volatile int32_t*>(static_cast<uintptr_t>(part + 52)) = k == 0 ? p.rep : p.cash;
                reinterpret_cast<int(__cdecl*)(uint32_t, uint32_t)>(Addr(kAddRewardPart))(acc, part);
                DeleteObj(part, 1);
            }
        }
        if (busted) return;
        const uint32_t ld = acc + 24;
        SetStr(ld + 4, "LD_CARD_GOLD");
        *reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(ld + 33)) = 1;
        if (g_luckyIndex < 0) g_luckyIndex = static_cast<int>(GetTickCount64() % (sizeof(kLucky) / sizeof(kLucky[0])));
        const Lucky& L = kLucky[g_luckyIndex];
        uint32_t item = reinterpret_cast<uint32_t(__cdecl*)()>(Addr(kNewLuckyItem))();
        if (!item) return;
        char desc[64];
        snprintf(desc, sizeof(desc), "%s x6", L.title);
        SetStr(item + 4, L.icon);
        SetStr(item + 20, desc);
        SetStr(item + 40, "POWERUP");
        *reinterpret_cast<volatile int32_t*>(static_cast<uintptr_t>(item + 72)) = 7;
        *reinterpret_cast<volatile int32_t*>(static_cast<uintptr_t>(item + 84)) = L.hash;
        reinterpret_cast<int(__cdecl*)(uint32_t, uint32_t)>(Addr(kAddLuckyItem))(ld, item);
        DeleteObj(item, 0);
        snprintf(luckyOut, static_cast<size_t>(luckyCap), "%s", desc);
    }

    static const char* ArgString(uint32_t base, uint32_t flagsOff, uint32_t ptrOff) {
        uint32_t flags = 0, p = 0;
        if (!ReadU32(base + flagsOff, flags) || !ReadU32(base + ptrOff, p)) return "";
        if (flags & 0x40) p = ReadU32Or(p, 0);
        return p ? reinterpret_cast<const char*>(static_cast<uintptr_t>(p)) : "";
    }

    using AccHandler_t = int(__cdecl*)(uint32_t*);
    static AccHandler_t g_origAccHandler = nullptr;

    static bool AnswerAccolades(uint32_t* a1) {
        Pursuit::Stats st = Pursuit::GetStats();
        const Rewards& r = ScreenRewards(st);
        char lucky[64] = "none";
        __try {
            uint32_t v10 = a1[0], sink = 0, v3 = a1[2];
            if (!v10 || !ReadU32(v10 + 8, sink) || !v3) return false;
            const char* cb1 = ArgString(v3, 4, 8);
            const char* cb2 = ArgString(v3, 20, 24);
            uint32_t pool = ReadU32Or(Addr(kGameAllocPool), 0);
            uint32_t* req = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(
                reinterpret_cast<uint32_t(__cdecl*)(int, void*, uint32_t, int, int, int)>(Addr(kGameAlloc))(40, nullptr, pool, 1, 0, 0)));
            if (!req) return false;
            auto fromC = reinterpret_cast<void(__thiscall*)(void*, const char*)>(Addr(kStringFromC));
            req[0] = Addr32(Ida::ArbRequestVtable); req[1] = sink;
            req[2] = req[3] = req[4] = 0; fromC(&req[2], cb1);
            req[6] = req[7] = req[8] = 0; fromC(&req[6], cb2);
            uint32_t acc = reinterpret_cast<uint32_t(__cdecl*)()>(Addr(kNewAccolades))();
            if (!acc) return false;
            FillAccolades(acc, r, st.busted, lucky, sizeof(lucky));
            reinterpret_cast<int(__thiscall*)(void*, uint32_t)>(Addr(Ida::ArbAnswer))(req, acc);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { LOG("[results] Accolades answer FAULTED"); return true; }
        LOG("[results] Accolades answered: %d cash, %d REP, %d part pair(s), lucky card %s (%s, level %d)", r.cash, r.rep, r.nParts, lucky,
            st.busted ? "busted" : "evaded", PlayerLevel());
        return true;
    }

    static int __cdecl AccoladesHook(uint32_t* a1) {
        if (!Pursuit::InResultsWindow() || !Features::On("accolades")) return g_origAccHandler(a1);
        LOG("[results] HandleRequest_Interop_Accolades (our pursuit -> answered here)");
        if (!AnswerAccolades(a1)) { LOG("[results] Accolades request unreadable -> game"); return g_origAccHandler(a1); }
        return 0;
    }

    void Tick(ULONGLONG now) {
        if (!g_arbCompletedDueMs || now < g_arbCompletedDueMs) return;
        g_arbCompletedDueMs = 0;
        if (!Pursuit::InResultsWindow() || !Features::On("arbsignal")) return;
        bool ok = true;
        __try { reinterpret_cast<int(__cdecl*)(int)>(Addr(kUiStateNative))(kUiArbitrationCompleted); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        LOG("[results] arbitration completed signalled (sub_69E280(%d)) %s", kUiArbitrationCompleted, ok ? "ok" : "FAULTED");
        (void)ok;
    }

    static bool g_committed = false;
    static char g_srvHost[64] = "127.0.0.1";
    static int g_srvPort = 3550;

    static void LoadServerConfig() {
        FILE* f = nullptr;
        if (fopen_s(&f, (ModuleDir() + "NFSWorldPursuitProbe_server.txt").c_str(), "r") != 0 || !f) return;
        char host[64] = {}; int port = 0;
        if (fscanf_s(f, "%63s %d", host, static_cast<unsigned>(sizeof(host)), &port) >= 1 && host[0]) {
            strcpy_s(g_srvHost, host);
            if (port > 0) g_srvPort = port;
        }
        fclose(f);
        LOG("[results] reward server %s:%d", g_srvHost, g_srvPort);
    }

    enum class Req { Reward, PowerupsReset, PowerupsGet };
    struct HttpJob { Req kind; wchar_t method[8]; wchar_t path[256]; };
    static volatile LONG g_profileRefresh = 0;
    static volatile ULONGLONG g_profileRefreshUntilMs = 0;

    static void ParsePowerups(const char* body) {
        UsedPowerup used[16] = {};
        int n = 0;
        for (const char* p = body; p && *p && n < 16;) {
            int hash = 0, count = 0;
            if (sscanf_s(p, "%d %d", &hash, &count) == 2 && count > 0) used[n++] = UsedPowerup{ hash, count };
            p = strchr(p, '\n');
            if (p) ++p;
        }
        EnterCriticalSection(&g_usedLock);
        memcpy(g_used, used, sizeof(used));
        g_usedCount = n;
        LeaveCriticalSection(&g_usedLock);
        LOG("[results] powerups used in this pursuit (server): %d kind(s)", n);
    }

    static DWORD WINAPI HttpThread(LPVOID p) {
        HttpJob* job = static_cast<HttpJob*>(p);
        wchar_t host[64] = {};
        MultiByteToWideChar(CP_ACP, 0, g_srvHost, -1, host, 64);
        DWORD status = 0;
        char body[2048] = {};
        DWORD got = 0;
        HINTERNET s = WinHttpOpen(L"NFSWorldPursuitProbe", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        HINTERNET c = s ? WinHttpConnect(s, host, static_cast<INTERNET_PORT>(g_srvPort), 0) : nullptr;
        HINTERNET r = c ? WinHttpOpenRequest(c, job->method, job->path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
        if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
            DWORD len = sizeof(status);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
            DWORD avail = 0;
            while (got < sizeof(body) - 1 && WinHttpQueryDataAvailable(r, &avail) && avail) {
                DWORD rd = 0, want = avail < sizeof(body) - 1 - got ? avail : sizeof(body) - 1 - got;
                if (!WinHttpReadData(r, body + got, want, &rd) || !rd) break;
                got += rd;
            }
        }
        body[got] = 0;
        switch (job->kind) {
        case Req::Reward:
            LOG("[results] rewards sent to the server: HTTP %lu%s", status, status == 200 ? "" : " (PursuitProbe.js installed? server running?)");
            if (status == 200) {
                LOG("[results] server: %s", body);
                InterlockedExchange(&g_leveledUp, strstr(body, "<LeveledUp>true</LeveledUp>") ? 1 : 0);
                if (g_leveledUp) LOG("[results] LEVEL UP -> HasLeveledUp set on the results screen");
                g_profileRefreshUntilMs = GetTickCount64() + 120000;
                InterlockedExchange(&g_profileRefresh, 4);
            }
            break;
        case Req::PowerupsReset:
            if (status != 200) LOG("[results] powerup counter reset: HTTP %lu (server patch up to date?)", status);
            break;
        case Req::PowerupsGet:
            if (status == 200) ParsePowerups(body);
            else LOG("[results] powerups used: HTTP %lu (server patch up to date?)", status);
            break;
        }
        if (r) WinHttpCloseHandle(r);
        if (c) WinHttpCloseHandle(c);
        if (s) WinHttpCloseHandle(s);
        delete job;
        return 0;
    }

    static void Http(Req kind, const wchar_t* method, const char* path) {
        HttpJob* job = new HttpJob{};
        job->kind = kind;
        wcsncpy_s(job->method, method, _TRUNCATE);
        MultiByteToWideChar(CP_ACP, 0, path, -1, job->path, 256);
        HANDLE t = CreateThread(nullptr, 0, HttpThread, job, 0, nullptr);
        if (t) CloseHandle(t); else delete job;
    }

    static void SendRewards(const Rewards& r, bool busted, float heat, int lucky) {
        char q[256];
        snprintf(q, sizeof(q), "/Engine.svc/pursuitprobe/reward?busted=%d&cash=%d&rep=%d&heat=%.2f&item=%s&qty=6", busted ? 1 : 0,
                 r.cash, r.rep, heat > 0 ? heat : 1.0f, (!busted && lucky >= 0) ? kLucky[lucky].tag : "");
        Http(Req::Reward, L"POST", q);
        LOG("[results] saving: %s", q);
    }

    void PursuitStarted() {
        g_committed = false; g_luckyIndex = -1; g_haveRewards = false;
        InterlockedExchange(&g_leveledUp, 0);
        EnterCriticalSection(&g_usedLock);
        g_usedCount = 0;
        LeaveCriticalSection(&g_usedLock);
        if (Features::On("powerupsused")) Http(Req::PowerupsReset, L"POST", "/Engine.svc/pursuitprobe/powerups/reset");
    }

    void ChaseOver() {
        if (Features::On("powerupsused")) Http(Req::PowerupsGet, L"GET", "/Engine.svc/pursuitprobe/powerups");
        Commit();
    }

    constexpr uint32_t kGetPersonaInfoAction = 0x4B89C0, kPersonaInfoRequest = 0x440320;
    using PersonaAction_t = void(__cdecl*)(int, int, unsigned);
    static PersonaAction_t g_origPersonaAction = nullptr;

    static void __cdecl PersonaActionHook(int movie, int args, unsigned argc) {
        if (argc >= 2 && g_profileRefresh > 0 && GetTickCount64() < g_profileRefreshUntilMs && Features::On("profilerefresh")) {
            struct { unsigned long long id; uint8_t force; uint8_t pad[7]; } b = {};
            __try {
                if ((*reinterpret_cast<uint8_t*>(args + 4) & 0x8F) == 3) b.id = static_cast<unsigned long long>(*reinterpret_cast<double*>(args + 8));
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { g_origPersonaAction(movie, args, argc); return; }
            b.force = 1;
            LONG left = InterlockedDecrement(&g_profileRefresh);
            LOG("[profile] GetPersonaInfo(%llu) forced from the server (%ld more)", b.id, left > 0 ? left : 0);
            (void)left;
            __try { reinterpret_cast<int(__cdecl*)(void*, int)>(Addr(kPersonaInfoRequest))(&b, movie); }
            __except (EXCEPTION_EXECUTE_HANDLER) { LOG("[profile] forced GetPersonaInfo FAULTED"); }
            return;
        }
        if (::Mod::Log::Enabled()) {
            if (argc >= 1) {
                double id = 0.0; uint8_t force = 0;
                __try {
                    if ((*reinterpret_cast<uint8_t*>(args + 4) & 0x8F) == 3) id = *reinterpret_cast<double*>(args + 8);
                    if (argc >= 2 && (*reinterpret_cast<uint32_t*>(args + 20) & 0x8F) == 2) force = *reinterpret_cast<uint8_t*>(args + 24);
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                LOG("[profile] UI GetPersonaInfo(%.0f, force %u)", id, force);
            }
        }
        g_origPersonaAction(movie, args, argc);
    }


    void Commit() {
        if (g_committed) return;
        g_committed = true;
        Pursuit::Stats st = Pursuit::GetStats();
        const Rewards& r = ScreenRewards(st);
        if (!st.busted && g_luckyIndex < 0) g_luckyIndex = static_cast<int>(GetTickCount64() % (sizeof(kLucky) / sizeof(kLucky[0])));
        SendRewards(r, st.busted, st.heat, g_luckyIndex);
    }

    using ArbHandler_t = int(__cdecl*)(int*);
    static ArbHandler_t g_origArbHandler = nullptr;

    static int __cdecl ArbPacketHook(int* args) {
        bool ours = Pursuit::InResultsWindow();
        LOG("[results] HandleRequest_Interop_ArbitratedPacket (%s)", ours ? "our pursuit -> answered here" : "not ours -> game");
        if (!ours) return g_origArbHandler(args);
        uint32_t req = BuildRequest(args);
        LOG("[results] request 0x%08X", req);
        if (req) Answer(req);
        if (!g_arbCompletedDueMs) g_arbCompletedDueMs = GetTickCount64() + kArbCompletedDelayMs;
        return 0;
    }

    void Install() {
        InitializeCriticalSection(&g_usedLock);
        LoadServerConfig();
        LoadTemplate();
        g_origArbHandler = reinterpret_cast<ArbHandler_t>(Addr(Ida::ArbPacketHandler));
        Hooks::Attach(reinterpret_cast<void**>(&g_origArbHandler), reinterpret_cast<void*>(ArbPacketHook), "arbitrated packet request");
        g_origCarDataSerialize = Addr32(Ida::CarDataSerialize);
        Hooks::Attach(reinterpret_cast<void**>(&g_origCarDataSerialize), reinterpret_cast<void*>(CarDataSerializeStub), "car data serializer");
        g_origPersonaSer = Addr32(0x492F70);
        Hooks::Attach(reinterpret_cast<void**>(&g_origPersonaSer), reinterpret_cast<void*>(PersonaSerStub), "persona serializer (level)");
        g_origAccHandler = reinterpret_cast<AccHandler_t>(Addr(kAccoladesHandler));
        Hooks::Attach(reinterpret_cast<void**>(&g_origAccHandler), reinterpret_cast<void*>(AccoladesHook), "accolades request");
        g_origEntrantSerialize = Addr32(Ida::EntrantSerialize);
        Hooks::Attach(reinterpret_cast<void**>(&g_origEntrantSerialize), reinterpret_cast<void*>(EntrantSerializeStub), "entrant serializer");
        g_origPersonaAction = reinterpret_cast<PersonaAction_t>(Addr(kGetPersonaInfoAction));
        Hooks::Attach(reinterpret_cast<void**>(&g_origPersonaAction), reinterpret_cast<void*>(PersonaActionHook), "GetPersonaInfo action (profile refresh)");
    }

    void Remove() {
        Hooks::Detach(reinterpret_cast<void**>(&g_origCarDataSerialize), reinterpret_cast<void*>(CarDataSerializeStub));
        Hooks::Detach(reinterpret_cast<void**>(&g_origEntrantSerialize), reinterpret_cast<void*>(EntrantSerializeStub));
        Hooks::Detach(reinterpret_cast<void**>(&g_origArbHandler), reinterpret_cast<void*>(ArbPacketHook));
        Hooks::Detach(reinterpret_cast<void**>(&g_origAccHandler), reinterpret_cast<void*>(AccoladesHook));
        Hooks::Detach(reinterpret_cast<void**>(&g_origPersonaSer), reinterpret_cast<void*>(PersonaSerStub));
        Hooks::Detach(reinterpret_cast<void**>(&g_origPersonaAction), reinterpret_cast<void*>(PersonaActionHook));
    }
}
