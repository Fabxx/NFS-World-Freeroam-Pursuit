#ifdef _DEBUG
// Debug-only: logs pursuit-breaker POI calls, PB/cooldown icon visibility and
// who switches PB/cooldown map entities on (entity+24), to compare a real
// pursuit event with ours. Signature-agnostic naked stubs, deduplicated.
#include "Log.h"
#include "Game.h"
#include "Hooks.h"
#include "Pursuit.h"
#include <mutex>
#include <cstdio>
#include <cstring>

namespace Mod::Log {

    // Selectors are icon-object methods: type = *(*(this+32) + field).
    struct ProbeTarget { uint32_t ida; const char* name; int field; };  // field -1: plain call log
    static const ProbeTarget kProbe[] = {
        { 0x799BC0, "map POI refresh (rebuilds PB + cooldown)", -1 },
        { 0x795190, "POI add pursuit breaker", -1 },
        { 0x795260, "POI remove pursuit breaker", -1 },
        { 0x71AD60, "pursuit cleanup (removes all PB)", -1 },
        { 0x4AC3B0, "PB/cooldown icon update", -2 },   // ecx = icon, a1 = entity: visible needs entity->vt[12]() && byte entity+64
        { 0x4AD640, "event icon update", -3 },         // same + sub_6CCBF0()/sub_6CCBA0() with data+93/+92
    };
    constexpr int kProbeCount = 6;
    constexpr int kMaxLinesPerTarget = 1500;  // icon hooks: only new (icon, state, FSM) combinations
    constexpr int kSeenSlots = 2048;

    static uint32_t g_probeOrig[kProbeCount] = {};
    static int g_probeLines[kProbeCount] = {};
    static uint32_t g_seen[kSeenSlots] = {};
    static std::mutex g_probeMutex;

    // Map layers: visible(entity) = entity+24 && layer(*(*(entity+28)))->+36, the layer
    // being looked up in the std::map at (*(dword_D11EF4)+4)+0x18 (node key +16, value +20).
    // Logged whenever any layer flag changes (throttled, game thread).
    static ULONGLONG g_layersNextMs = 0;
    static uint32_t g_layerKey[64] = {}, g_layerFlag[64] = {};
    static int g_layerCount = -1;

    static void DumpLayers(uint32_t fsm, bool ours) {
        ULONGLONG now = GetTickCount64();
        if (now < g_layersNextMs) return;
        g_layersNextMs = now + 250;
        uint32_t mgr = 0, obj = 0;
        if (!ReadU32(Addr(0xD11EF4), mgr) || !mgr || !ReadU32(mgr + 4, obj) || !obj) return;
        uint32_t map = obj + 0x18, head = map + 4, root = 0;
        if (!ReadU32(map + 12, root)) return;
        uint32_t stack[64], keys[64], flags[64];
        int sp = 0, n = 0;
        if (root && root != head) stack[sp++] = root;
        while (sp && n < 64) {
            uint32_t node = stack[--sp], key = 0, val = 0, l = 0, r = 0, f = 0xFF;
            if (!ReadU32(node + 16, key) || !ReadU32(node + 20, val)) continue;
            if (val && ReadU32(val + 36, f)) f &= 0xFF;
            keys[n] = key; flags[n] = f; ++n;
            if (ReadU32(node, l) && l && l != head && sp < 64) stack[sp++] = l;
            if (ReadU32(node + 4, r) && r && r != head && sp < 64) stack[sp++] = r;
        }
        bool changed = n != g_layerCount;
        for (int i = 0; i < n && !changed; ++i) changed = keys[i] != g_layerKey[i] || flags[i] != g_layerFlag[i];
        if (!changed) return;
        char buf[900]; int len = 0;
        for (int i = 0; i < n && len < 860; ++i) len += sprintf_s(buf + len, sizeof(buf) - len, " %08X=%u", keys[i], flags[i]);
        Write("[layers] FSM=%u %s%s", fsm, ours ? "[OUR pursuit]" : "", len ? buf : " (empty)");
        memcpy(g_layerKey, keys, n * 4); memcpy(g_layerFlag, flags, n * 4); g_layerCount = n;
    }

    static uint32_t Mix(uint32_t h, uint32_t v) { h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2); return h; }

    static void __stdcall ProbeLog(int idx, uint32_t ecx, uint32_t ret, uint32_t a1, uint32_t a2, uint32_t a3) {
        const ProbeTarget& t = kProbe[idx];
        uint32_t fe = 0xFFFFFFFF;
        uint32_t data = 0;
        if (ecx >= 0x10000 && ReadU32(ecx + 32, data) && data >= 0x10000) {
            if (t.field >= 0) ReadU32(data + t.field, fe);
            else if (t.field == -2) ReadU32(data + 100, fe);
            else if (t.field == -3) ReadU32(data + 80, fe);
        }
        uint32_t fsm = ReadU32Or(Addr(Ida::Fsm), 0xFFFFFFFF);
        bool ours = Pursuit::IsActive();
        uint32_t layer = 0, lp = 0;
        if (t.field <= -2 && a1 >= 0x10000 && ReadU32(a1 + 28, lp) && lp) ReadU32(lp, layer);
        uint32_t ent64 = 0xFF, d92 = 0xFFFF, gA = 0xFF, gB = 0xFF, vt = 0, fn = 0;
        if (t.field <= -2) {
            uint32_t w = 0, r12 = 0;
            if (a1 >= 0x10000 && ReadU32(a1 + 64, w)) ent64 = w & 0xFF;
            // entity->vt[12]() (+48): the other visibility condition, folded into bit 8
            if (a1 >= 0x10000 && ReadU32(a1, vt) && ReadU32(vt + 48, fn) && fn && TryCallAny1(fn, a1, a1, r12))
                ent64 |= (r12 & 0xFF) ? 0x100 : 0;
            if (t.field == -3) {
                if (data >= 0x10000 && ReadU32(data + 92, w)) d92 = w & 0xFFFF;
                uint32_t r = 0;
                if (TryCallAny1(Addr32(0x6CCBF0), 0, 0, r)) gA = r & 0xFF;
                if (TryCallAny1(Addr32(0x6CCBA0), 0, 0, r)) gB = r & 0xFF;
            }
        }
        std::lock_guard<std::mutex> lock(g_probeMutex);
        if (g_probeLines[idx] >= kMaxLinesPerTarget) return;
        if (t.field != -1) {
            uint32_t h = Mix(Mix(Mix(Mix(Mix(Mix(Mix(static_cast<uint32_t>(idx), ecx), fe), fsm), ours ? 1u : 0u), ent64), d92), (gA << 8) | gB);
            if (!h) h = 1;
            uint32_t slot = h % kSeenSlots;
            for (int probe = 0; probe < 16; ++probe, slot = (slot + 1) % kSeenSlots) {
                if (g_seen[slot] == h) return;
                if (!g_seen[slot]) { g_seen[slot] = h; break; }
            }
        }
        ++g_probeLines[idx];
        uintptr_t mb = ExeBase();
        uint32_t retIda = (ret >= mb && ret < mb + 0x900000) ? ret - static_cast<uint32_t>(mb) + 0x400000 : ret;
        if (t.field == -2)
            Write("[map] %s type=%d layer=%08X entity=0x%08X (vt IDA 0x%08X, vt12 IDA 0x%08X) entity+64=%u vt12=%u FSM=%u %s", t.name,
                static_cast<int32_t>(fe), layer, a1, vt ? vt - static_cast<uint32_t>(mb) + 0x400000 : 0, fn ? fn - static_cast<uint32_t>(mb) + 0x400000 : 0,
                ent64 & 0xFF, ent64 >> 8, fsm,
                ours ? "[OUR pursuit]" : "");
        else if (t.field == -3)
            Write("[map] %s type=%d layer=%08X entity=0x%08X (vt IDA 0x%08X, vt12 IDA 0x%08X) entity+64=%u vt12=%u data+92/93=%02X/%02X sub_6CCBF0=%u sub_6CCBA0=%u FSM=%u %s", t.name,
                static_cast<int32_t>(fe), layer, a1, vt ? vt - static_cast<uint32_t>(mb) + 0x400000 : 0, fn ? fn - static_cast<uint32_t>(mb) + 0x400000 : 0, ent64 & 0xFF, ent64 >> 8, d92 & 0xFF, (d92 >> 8) & 0xFF, gA, gB, fsm, ours ? "[OUR pursuit]" : "");
        else if (t.field >= 0)
            Write("[map] %s = %d  icon=0x%08X data=0x%08X ret=IDA 0x%08X FSM=%u %s", t.name, static_cast<int32_t>(fe), ecx, data,
                retIda, fsm, ours ? "[OUR pursuit]" : "");
        else
            Write("[map] %s ret=IDA 0x%08X ecx=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X FSM=%u %s", t.name, retIda, ecx, a1, a2, a3,
                fsm, ours ? "[OUR pursuit]" : "");
    }

    // [esp] after pushfd+pushad: +36 ret, +40 a1, +44 a2, +48 a3; each push moves esp by 4.
#define MAP_PROBE_STUB(N) \
    static __declspec(naked) void ProbeStub##N() { \
        __asm pushfd \
        __asm pushad \
        __asm push dword ptr [esp + 48] \
        __asm push dword ptr [esp + 48] \
        __asm push dword ptr [esp + 48] \
        __asm push dword ptr [esp + 48] \
        __asm push ecx \
        __asm push N \
        __asm call ProbeLog \
        __asm popad \
        __asm popfd \
        __asm jmp dword ptr [g_probeOrig + 4 * N] \
    }
    MAP_PROBE_STUB(0)
    MAP_PROBE_STUB(1)
    MAP_PROBE_STUB(2)
    MAP_PROBE_STUB(3)
    MAP_PROBE_STUB(4)
    MAP_PROBE_STUB(5)
    static void (*const kProbeStubs[kProbeCount])() = { ProbeStub0, ProbeStub1, ProbeStub2, ProbeStub3, ProbeStub4, ProbeStub5 };

    // sub_773610 (entity->vt[11], sets entity+24) is shared by many classes: only PB/cooldown
    // entities (vt 0xBC2210) are logged, with the return addresses found on the stack
    // (the direct callers 0x68B766 / 0x68B786 are one-line wrappers).
    static uint32_t g_setterOrig = 0;

    // Separate function: __try is not allowed where objects need unwinding (C2712).
    static int StackChain(const uint32_t* frame, uint32_t lo, uint32_t hi, uint32_t (&chain)[8]) {
        int n = 0;
        __try {
            chain[n++] = frame[0];
            for (int i = 2; i < 160 && n < 8; ++i) if (frame[i] >= lo && frame[i] < hi) chain[n++] = frame[i];
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return n;
    }

    static void __stdcall SetterLog(uint32_t ecx, const uint32_t* frame) {
        uint32_t vt = 0, kind = 0xFFFFFFFF;
        if (ecx < 0x10000 || !ReadU32(ecx, vt) || vt != Addr32(0xBC2210)) return;
        ReadU32(ecx + 100, kind);
        const uint32_t mb = static_cast<uint32_t>(ExeBase()), lo = mb + 0x1000, hi = mb + 0x800000;
        uint32_t chain[8] = {};
        int n = StackChain(frame, lo, hi, chain);
        uint32_t show = frame[1] & 0xFF, fsm = ReadU32Or(Addr(Ida::Fsm), 0xFFFFFFFF);
        uint32_t h = Mix(Mix(Mix(Mix(99u, show), kind), fsm), Pursuit::IsActive() ? 1u : 0u);
        for (int i = 0; i < n && i < 4; ++i) h = Mix(h, chain[i]);
        std::lock_guard<std::mutex> lock(g_probeMutex);
        if (!h) h = 1;
        uint32_t slot = h % kSeenSlots;
        for (int probe = 0; probe < 16; ++probe, slot = (slot + 1) % kSeenSlots) {
            if (g_seen[slot] == h) return;
            if (!g_seen[slot]) { g_seen[slot] = h; break; }
        }
        char buf[160]; int len = 0;
        for (int i = 0; i < n; ++i) len += sprintf_s(buf + len, sizeof(buf) - len, " %08X", chain[i] - mb + 0x400000);
        Write("[map] PB/cooldown set visible %s -> %u entity=0x%08X FSM=%u %s| stack (IDA):%s", kind == 1 ? "BREAKER" : kind == 0 ? "COOLDOWN" : "?",
              show, ecx, fsm, Pursuit::IsActive() ? "[OUR pursuit] " : "", buf);
    }

    // after pushfd+pushad: [esp+36] = ret, [esp+40] = a1
    static __declspec(naked) void SetterStub() {
        __asm pushfd
        __asm pushad
        __asm lea eax, [esp + 36]
        __asm push eax
        __asm push ecx
        __asm call SetterLog
        __asm popad
        __asm popfd
        __asm jmp dword ptr [g_setterOrig]
    }

    // AIPursuit (CopMgr+0x1E0) snapshot diff, to find the cooldown state: logs fields whose
    // value changes between "flag-like" values (both <= 16, or to/from 0); a field that keeps
    // changing is muted after kPsMaxChanges. Lined up with the script's COOLDOWN -> 1 / 0.
    constexpr int kPsBefore = 0x80, kPsDwords = 0x800 / 4, kPsMaxChanges = 10, kPsMaxLines = 800;
    static uint32_t g_ps[kPsDwords], g_psObj = 0;
    static uint8_t g_psChanges[kPsDwords];
    static int g_psLines = 0;

    static void DiffPursuit(uint32_t fsm, bool ours) {
        uint32_t icop = 0, p = 0;
        if (!ReadU32(Addr(Ida::CopMgrPtr), icop) || !icop || !ReadU32(icop + 0x1E0, p) || !p) {
            if (g_psObj) Write("[aipursuit] gone (FSM=%u)", fsm);
            g_psObj = 0;
            return;
        }
        uint32_t cur[kPsDwords];
        const uint32_t base = p - kPsBefore;
        for (int i = 0; i < kPsDwords; ++i) if (!ReadU32(base + i * 4, cur[i])) cur[i] = 0xDEADBEEF;
        if (p != g_psObj) {
            uint32_t vt = 0; ReadU32(p, vt);
            Write("[aipursuit] new 0x%08X vt IDA 0x%08X FSM=%u %s", p, vt ? vt - static_cast<uint32_t>(ExeBase()) + 0x400000 : 0,
                  fsm, ours ? "[OUR pursuit]" : "");
            g_psObj = p;
            memcpy(g_ps, cur, sizeof(cur));
            memset(g_psChanges, 0, sizeof(g_psChanges));
            return;
        }
        char buf[1000]; int len = 0;
        for (int i = 0; i < kPsDwords && len < 900; ++i) {
            uint32_t o = g_ps[i], n = cur[i];
            if (o == n || g_psChanges[i] >= kPsMaxChanges) continue;
            if (!((o <= 16 && n <= 16) || o == 0 || n == 0)) continue;
            ++g_psChanges[i];
            float fo, fn; memcpy(&fo, &o, 4); memcpy(&fn, &n, 4);
            len += sprintf_s(buf + len, sizeof(buf) - len, " %+d: %X->%X (%g->%g)%s", i * 4 - kPsBefore, o, n, fo, fn,
                             g_psChanges[i] == kPsMaxChanges ? " [muted]" : "");
        }
        memcpy(g_ps, cur, sizeof(cur));
        if (len && g_psLines < kPsMaxLines) { ++g_psLines; Write("[aipursuit] FSM=%u %s|%s", fsm, ours ? "[OUR pursuit] " : "", buf); }
    }


    void PollMapLayers() {
        uint32_t fsm = ReadU32Or(Addr(Ida::Fsm), 0xFFFFFFFF);
        bool ours = Pursuit::IsActive();
        std::lock_guard<std::mutex> l(g_probeMutex);
        DumpLayers(fsm, ours);
        DiffPursuit(fsm, ours);
    }

    void InstallMapProbe() {
        for (int i = 0; i < kProbeCount; ++i) {
            g_probeOrig[i] = Addr32(kProbe[i].ida);
            Hooks::Attach(reinterpret_cast<void**>(&g_probeOrig[i]), reinterpret_cast<void*>(kProbeStubs[i]), kProbe[i].name);
        }
        g_setterOrig = Addr32(0x773610);
        Hooks::Attach(reinterpret_cast<void**>(&g_setterOrig), reinterpret_cast<void*>(SetterStub), "PB/cooldown entity set visible");
    }

    void RemoveMapProbe() {
        for (int i = 0; i < kProbeCount; ++i)
            Hooks::Detach(reinterpret_cast<void**>(&g_probeOrig[i]), reinterpret_cast<void*>(kProbeStubs[i]));
        Hooks::Detach(reinterpret_cast<void**>(&g_setterOrig), reinterpret_cast<void*>(SetterStub));
    }
}
#endif
