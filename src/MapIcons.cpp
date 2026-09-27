#include "MapIcons.h"
#include "Game.h"
#include "Log.h"

namespace Mod::MapIcons {

    constexpr uint32_t kMapManager = 0xD11EF4;
    constexpr uint32_t kLayerLookup = 0x6CD920;   // thiscall(std::map*, &key) -> &value (layer*)
    constexpr uint32_t kEventLayer = 1;
    constexpr uint32_t kLayerVisible = 36;        // byte

    static bool g_hidden = false;
    static uint8_t g_saved = 1;

    // obj = *(manager+4): +8 / +12 the two views, +24 the layer map.
    static bool Resolve(uint32_t& obj, uint32_t& layer) {
        uint32_t mgr = 0, key = kEventLayer, slot = 0;
        if (!ReadU32(Addr(kMapManager), mgr) || !mgr || !ReadU32(mgr + 4, obj) || !obj) return false;
        if (!TryCallAny1(Addr32(kLayerLookup), obj + 24, reinterpret_cast<uint32_t>(&key), slot) || !slot) return false;
        return ReadU32(slot, layer) && layer;
    }

    // Same as the game (listener sub_6CE7D0 / binding sub_6CE070): layer->+36 = show, then
    // view->vt[7](layer, show) on both views. View A (obj+8, UI) also drives the icons drawn
    // over the 3D world; view B's vt[7] is empty. (The clean10 crash blamed on this was the music.)
    static void CallViews(uint32_t obj, uint8_t show) {
        constexpr uint32_t kViewSetLayerVt = 28;
        const uint32_t offs[2] = { 8u, 12u };
        for (uint32_t off : offs) {
            uint32_t view = 0, vt = 0, fn = 0;
            if (!ReadU32(obj + off, view) || !view || !ReadU32(view, vt) || !ReadU32(vt + kViewSetLayerVt, fn) || !fn) continue;
            __try {
                reinterpret_cast<void(__thiscall*)(void*, int, int)>(fn)(reinterpret_cast<void*>(static_cast<uintptr_t>(view)),
                                                                        static_cast<int>(kEventLayer), show);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    static void SetLayer(uint32_t obj, uint32_t layer, uint8_t show) {
        __try { *reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(layer + kLayerVisible)) = show; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        CallViews(obj, show);
    }

    // ---- layer 7 entities (pursuit breakers / cooldown spots)
    constexpr uint32_t kPoiEntityVt = 0xBC2210;
    constexpr uint32_t kEntityList = 52, kEntityVisible = 24, kEntityKind = 100;
    enum : uint32_t { kKindCooldown = 0, kKindBreaker = 1 };
    constexpr int kMaxTouched = 512;
    static uint32_t g_touched[kMaxTouched];
    static int g_touchedCount = 0;


    static void Remember(uint32_t e) {
        for (int i = 0; i < g_touchedCount; ++i) if (g_touched[i] == e) return;
        if (g_touchedCount < kMaxTouched) g_touched[g_touchedCount++] = e;
    }

    // Sets entity+24 on every PB/cooldown entity of the given kind; remembers the ones switched on.
    static int ShowKind(uint32_t kind, uint8_t show) {
        uint32_t mgr = 0, obj = 0, it = 0, end = 0;
        if (!ReadU32(Addr(kMapManager), mgr) || !mgr || !ReadU32(mgr + 4, obj) || !obj) return -1;
        if (!ReadU32(obj + kEntityList, it) || !ReadU32(obj + kEntityList + 4, end) || !it || end < it || end - it > 0x10000) return -1;
        const uint32_t vtWant = Addr32(kPoiEntityVt);
        int n = 0;
        __try {
            for (; it < end; it += 4) {
                uint32_t e = 0, vt = 0, k = 0;
                if (!ReadU32(it, e) || !e || !ReadU32(e, vt) || vt != vtWant || !ReadU32(e + kEntityKind, k) || k != kind) continue;
                volatile uint8_t* flag = reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(e + kEntityVisible));
                if (*flag == show) continue;
                *flag = show;
                ++n;
                if (show) Remember(e);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return n;
    }

    static void HidePoiTouched() {
        const uint32_t vtWant = Addr32(kPoiEntityVt);
        for (int i = 0; i < g_touchedCount; ++i) {
            uint32_t vt = 0;
            if (!ReadU32(g_touched[i], vt) || vt != vtWant) continue;   // map rebuilt meanwhile
            __try { *reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(g_touched[i] + kEntityVisible)) = 0; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        g_touchedCount = 0;
    }

    static void HideEvents() {
        uint32_t obj = 0, layer = 0, cur = 0;
        if (g_hidden || !Resolve(obj, layer) || !ReadU32(layer + kLayerVisible, cur)) { LOG("[map] event layer not found"); return; }
        g_saved = static_cast<uint8_t>(cur & 0xFF);
        g_hidden = true;
        if (g_saved) SetLayer(obj, layer, 0);
        LOG("[map] event icons hidden (layer was %u)", g_saved);
    }

    static void RestoreEvents() {
        if (!g_hidden) return;
        g_hidden = false;
        uint32_t obj = 0, layer = 0;
        if (!Resolve(obj, layer)) return;   // map rebuilt meanwhile: it restored itself
        SetLayer(obj, layer, g_saved);
        LOG("[map] event icons restored (%u)", g_saved);
    }

    void EnterPursuit() {
        HideEvents();
        int n = ShowKind(kKindBreaker, 1);
        LOG("[map] pursuit breakers shown: %d", n);
        (void)n;
    }

    // Real event: cooldown start = breakers off + hiding spots on; back to the chase = the reverse.
    void SetCooldown(bool on) {
        int b = ShowKind(kKindBreaker, on ? 0 : 1);
        int c = ShowKind(kKindCooldown, on ? 1 : 0);
        LOG("[map] cooldown %s: breakers %d, hiding spots %d", on ? "ON" : "OFF", b, c);
        (void)b; (void)c;
    }

    void HidePursuitIcons() {
        HidePoiTouched();
        LOG("[map] pursuit breakers / hiding spots hidden");
    }

    void ExitPursuit() {
        HidePoiTouched();
        RestoreEvents();
    }
}
