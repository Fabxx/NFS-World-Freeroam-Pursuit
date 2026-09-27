#include "Results.h"
#include "Game.h"
#include "Hooks.h"
#include "Pursuit.h"
#include "Log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Mod::Results {

    // PursuitEntrantArbitratedPacket (serializer sub_499470)
    constexpr int kEntrantWords = 270;   // 1080 bytes
    constexpr int kPursuitBlockWord = 260;  // +1040.. = our own stats, never from the template
    namespace E {
        constexpr uint32_t Rank = 188, FinishReason = 192, Duration = 196, TimeDiff = 200, HasArbitrated = 220,
            NumPowerups = 224, CopsDeployed = 1040, CopsDisabled = 1044, CopsRammed = 1048, CostToState = 1052,
            Heat = 1056, Infractions = 1060, LongestJump = 1064, RoadBlocks = 1068, SpikeStrips = 1072, TopSpeed = 1076;
    }
    constexpr uint32_t kPacketEntrant = 64, kPacketHasArbitrated = 60;
    constexpr uint32_t kFinishEvaded = 0x206;   // value of a real evaded result
    constexpr uint32_t kCostPerCop = 250;

    // Template: kind 1 = plain value (int, float, hash), 2 = start of a
    // std::string {begin, end, cap} whose text is in g_str.
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

    // Built-in entrant (captured from a real pursuit results screen) used when
    // the file is missing. Without these fields the results screen never
    // accepts the packet and keeps asking ("please wait"). A real pursuit
    // results screen replaces them with the current player / car.
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
        if (n > 0) g_valid = true;
        else LoadDefaultTemplate();
    }

    // ------------------------------------------------------------ capture
    static bool Readable(uint32_t a) {
        if (a < 0x10000) return false;
        __try { volatile uint8_t b = *reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(a)); (void)b; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool CopyBytes(void* dst, uint32_t src, uint32_t n) {
        __try { memcpy(dst, reinterpret_cast<const void*>(static_cast<uintptr_t>(src)), n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // Classified while the real packet is alive: non-address words are
    // values, {begin, end, cap} pointer triplets are strings (text copied).
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
        if (Pursuit::InResultsWindow()) return;   // our own packet
        static uint32_t w[kEntrantWords];
        static uint8_t kind[kEntrantWords];
        static char str[kEntrantWords][64];
        if (!Capture(entrant, kEntrantWords, w, kind, str)) return;
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

    // The current car: the game serializes the active car's CarData (vtable
    // 0xB26524, serializer sub_4973C0) for the safehouse (Interop_SafeHouseActiveCar,
    // call site 0x494952) and for HandleRequest_Interop_CurrentCarInfo
    // (answer sub_51B0E0, call site 0x51B138) -- at login, entering freeroam
    // and after every car change. That copy replaces the CarData block of
    // the template (entrant +0xE8..+0x40F), so our results follow the car.
    constexpr uint32_t kCarDataWord = 0xE8 / 4;
    constexpr int kCarDataWords = kPursuitBlockWord - kCarDataWord;

    static void __stdcall OnCarDataSerialize(uint32_t carData, uint32_t ret) {
        uint32_t site = static_cast<uint32_t>(ret - ExeBase() + 0x400000);
        // Not gated on InResultsWindow(): these two call sites never serialize
        // our packet, and a car change often happens within 30s of a pursuit.
        if (site != 0x494952 && site != 0x51B138) return;
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

    // ------------------------------------------------------------ answer
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
                // player / car fields of the real entrant
                for (int i = 1; i < kPursuitBlockWord; ++i) {
                    if (g_kind[i] == 1 && e[i] == 0) e[i] = g_words[i];
                    else if (g_kind[i] == 2 && g_str[i][0] && e[i] == empty && e[i + 1] == empty) {
                        AssignString(ent + 4 * i, g_str[i]);
                        i += 2;
                    }
                }
                // HAT ratings {vt, float n/1000, int n, string "n"}: float and int
                // are rebuilt from the digits (a float can look like an address).
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
            if (!e[E::FinishReason / 4]) e[E::FinishReason / 4] = kFinishEvaded;
            *F32(ent, E::Duration) = duration;
            *F32(ent, E::TimeDiff) = 0.0f;
            *reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(ent + E::HasArbitrated)) = 1;
            e[E::NumPowerups / 4] = 0;
            e[E::CopsDeployed / 4] = cops;
            e[E::CopsDisabled / 4] = 0;
            e[E::CopsRammed / 4] = 0;
            e[E::CostToState / 4] = cops * kCostPerCop;
            *F32(ent, E::Heat) = 1.0f;
            e[E::Infractions / 4] = 0;
            *F32(ent, E::LongestJump) = 0.0f;
            e[E::RoadBlocks / 4] = 0;
            e[E::SpikeStrips / 4] = 0;
            *F32(ent, E::TopSpeed) = 0.0f;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        LOG("[results] packet 0x%08X: duration %.1fs, %u cop(s), template %s", packet, duration, cops, g_valid ? "yes" : "no");
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

    using ArbHandler_t = int(__cdecl*)(int*);
    static ArbHandler_t g_origArbHandler = nullptr;

    static int __cdecl ArbPacketHook(int* args) {
        bool ours = Pursuit::InResultsWindow();
        LOG("[results] HandleRequest_Interop_ArbitratedPacket (%s)", ours ? "our pursuit -> answered here" : "not ours -> game");
        if (!ours) return g_origArbHandler(args);
        uint32_t req = BuildRequest(args);
        LOG("[results] request 0x%08X", req);
        if (req) Answer(req);
        return 0;
    }

    void Install() {
        LoadTemplate();
        g_origArbHandler = reinterpret_cast<ArbHandler_t>(Addr(Ida::ArbPacketHandler));
        Hooks::Attach(reinterpret_cast<void**>(&g_origArbHandler), reinterpret_cast<void*>(ArbPacketHook), "arbitrated packet request");
        g_origCarDataSerialize = Addr32(Ida::CarDataSerialize);
        Hooks::Attach(reinterpret_cast<void**>(&g_origCarDataSerialize), reinterpret_cast<void*>(CarDataSerializeStub), "car data serializer");
        g_origEntrantSerialize = Addr32(Ida::EntrantSerialize);
        Hooks::Attach(reinterpret_cast<void**>(&g_origEntrantSerialize), reinterpret_cast<void*>(EntrantSerializeStub), "entrant serializer");
    }

    void Remove() {
        Hooks::Detach(reinterpret_cast<void**>(&g_origCarDataSerialize), reinterpret_cast<void*>(CarDataSerializeStub));
        Hooks::Detach(reinterpret_cast<void**>(&g_origEntrantSerialize), reinterpret_cast<void*>(EntrantSerializeStub));
        Hooks::Detach(reinterpret_cast<void**>(&g_origArbHandler), reinterpret_cast<void*>(ArbPacketHook));
    }
}
