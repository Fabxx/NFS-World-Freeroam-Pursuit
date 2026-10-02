// "Freeroam pursuits" switch shown in Options > Gameplay.
// The switch reuses the "moments" field of the game's own GamePlayOptions (camera, transmission, damage,
// moments, speedUnits): the game never applies "moments", but it keeps it, sends it to Options.gfx,
// saves it in UserSettings.xml (UI/Gameplay) and restores it on Cancel, all with its own requests
// (GamePlaySetMoments / SaveOptions / CancelChanges / ResetToDefault). The ASI only follows the value.
#include "Options.h"
#include "Game.h"
#include "Hooks.h"
#include "Log.h"
#include <atomic>
#include <cstring>

namespace Mod::Options {
    constexpr uint32_t kOptionsSlot = 0xCE334C, kOptionsVtable = 0xB8443C;
    constexpr uint32_t kMomentsOffset = 28;
    constexpr uint32_t kReqSave = 0x4B8CA0, kReqGet = 0x4B8CC0, kReqReset = 0x4B8CE0, kReqMoments = 0x4B8DC0, kReqCancel = 0x4B8E40;
    constexpr uint32_t kSettingsMgr = 0x656320;
    constexpr int kSettingsGetXml = 68;
    constexpr ULONGLONG kLoadDelayMs = 3000;
    constexpr bool kDefault = true;

    static std::atomic<bool> g_armed{ false };
    static bool g_loaded = false;
    static ULONGLONG g_firstTickMs = 0;

    static void Set(bool on, const char* why) {
        if (g_armed.exchange(on) != on || !g_loaded) LOG("[options] freeroam pursuits %s (%s)", on ? "ON" : "OFF", why);
        g_loaded = true;
        (void)why;
    }

    // The gameplay options object (vtable 0xB8443C) is reached from 0xCE334C either directly or through
    // one more pointer; check the vtable instead of trusting either layout.
    static uint32_t OptionsObject() {
        const uint32_t vt = Addr32(kOptionsVtable);
        uint32_t p = 0, w = 0, q = 0;
        if (!ReadU32(Addr(kOptionsSlot), p) || !p || !ReadU32(p, w)) return 0;
        if (w == vt) return p;
        return w && ReadU32(w, q) && q == vt ? w : 0;
    }

    static bool ReadMoments(bool& on) {
        uint32_t obj = OptionsObject(), m = 0;
        if (!obj || !ReadU32(obj + kMomentsOffset, m)) return false;
        on = m != 0;
        return true;
    }

    static void Sync(const char* why) {
        bool on = false;
        if (ReadMoments(on)) Set(on, why);
        else LOG("[options] %s: gameplay options object not found (0x%08X)", why, OptionsObject());
    }

    // UserSettings.xml, UI/Gameplay: <GamePlayOptions ... moments="1" ... />
    static bool ReadXml(bool& on) {
        uint32_t mgr = 0, vt = 0, fn = 0;
        __try { mgr = reinterpret_cast<uint32_t(__cdecl*)()>(Addr(kSettingsMgr))(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        if (!mgr || !ReadU32(mgr, vt) || !ReadU32(vt + kSettingsGetXml, fn) || !fn) return false;
        __try {
            const char* xml = reinterpret_cast<const char*(__thiscall*)(uint32_t, const char*, const char*)>(fn)(mgr, "UI", "Gameplay");
            if (!xml || xml[0] != '<') return false;
            const char* p = strstr(xml, "moments");
            if (!p) return false;
            p += 7;
            for (int i = 0; i < 8 && *p && (*p < '0' || *p > '9'); ++i) ++p;
            if (*p < '0' || *p > '9') return false;
            on = *p != '0';
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static void LoadAtStart() {
        bool on = kDefault;
        const bool ok = ReadXml(on);
        Set(on, ok ? "UserSettings.xml" : "default, no gameplay settings saved yet");
    }

    using Req_t = int(__cdecl*)(int, int, int);
    static Req_t g_origSave = nullptr, g_origGet = nullptr, g_origReset = nullptr, g_origMoments = nullptr, g_origCancel = nullptr;

    static int __cdecl SaveHook(int a, int b, int c) { int r = g_origSave(a, b, c); Sync("options applied"); return r; }
    static int __cdecl GetHook(int a, int b, int c) { int r = g_origGet(a, b, c); Sync("options read"); return r; }
    static int __cdecl ResetHook(int a, int b, int c) { int r = g_origReset(a, b, c); Sync("options reset"); return r; }
    static int __cdecl MomentsHook(int a, int b, int c) { int r = g_origMoments(a, b, c); Sync("Options > Gameplay"); return r; }
    static int __cdecl CancelHook(int a, int b, int c) { int r = g_origCancel(a, b, c); Sync("options cancelled"); return r; }

    bool Armed() { return g_armed.load(); }

    void Tick(ULONGLONG now) {
        if (!g_firstTickMs) g_firstTickMs = now;
        if (!g_loaded && now - g_firstTickMs >= kLoadDelayMs) LoadAtStart();
    }

    static void Attach(Req_t& orig, uint32_t ida, void* hook, const char* name) {
        orig = reinterpret_cast<Req_t>(Addr(ida));
        Hooks::Attach(reinterpret_cast<void**>(&orig), hook, name);
    }

    static void Detach(Req_t& orig, void* hook) {
        if (orig) Hooks::Detach(reinterpret_cast<void**>(&orig), hook);
    }

    void Install() {
        Attach(g_origSave, kReqSave, reinterpret_cast<void*>(SaveHook), "GamePlaySaveOptions");
        Attach(g_origGet, kReqGet, reinterpret_cast<void*>(GetHook), "GamePlayGetOptions");
        Attach(g_origReset, kReqReset, reinterpret_cast<void*>(ResetHook), "GamePlayResetToDefault");
        Attach(g_origMoments, kReqMoments, reinterpret_cast<void*>(MomentsHook), "GamePlaySetMoments (freeroam pursuits)");
        Attach(g_origCancel, kReqCancel, reinterpret_cast<void*>(CancelHook), "GamePlayCancelChanges");
    }

    void Remove() {
        Detach(g_origSave, reinterpret_cast<void*>(SaveHook));
        Detach(g_origGet, reinterpret_cast<void*>(GetHook));
        Detach(g_origReset, reinterpret_cast<void*>(ResetHook));
        Detach(g_origMoments, reinterpret_cast<void*>(MomentsHook));
        Detach(g_origCancel, reinterpret_cast<void*>(CancelHook));
    }
}
