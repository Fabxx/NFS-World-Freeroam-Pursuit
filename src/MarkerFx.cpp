// In-world event markers (activities): suspended during our pursuit, resumed afterwards.
#include "MarkerFx.h"
#include "Game.h"
#include "Hooks.h"
#include "Log.h"

namespace Mod::MarkerFx {
    constexpr uint32_t kCtorSingle = 0x6882F0, kCtorDual = 0x688400;
    constexpr uint32_t kDtorSingle = 0x694F90, kDtorDual = 0x6950E0;
    constexpr uint32_t kVtSingle = 0xBB8D94, kVtDual = 0xBB8DC4;
    constexpr uint32_t kSetSuspended = 0x774CA0;
    constexpr uint32_t kSuspendedByte = 37;
    constexpr int kMax = 4096;

    struct Slot { uint32_t obj; bool ours; bool spot; };
    static Slot g_slots[kMax];
    static int g_count = 0;
    static SRWLOCK g_lock = SRWLOCK_INIT;
    static bool g_hidden = false;

    static uint32_t g_ctorSingle = 0, g_ctorDual = 0, g_dtorSingle = 0, g_dtorDual = 0;

    static void __stdcall OnCreate(uint32_t obj) {
        AcquireSRWLockExclusive(&g_lock);
        int free = -1;
        for (int i = 0; i < g_count; ++i) {
            if (g_slots[i].obj == obj) { free = i; break; }
            if (!g_slots[i].obj && free < 0) free = i;
        }
        if (free < 0 && g_count < kMax) free = g_count++;
        if (free >= 0) { g_slots[free].obj = obj; g_slots[free].ours = false; g_slots[free].spot = false; }
        ReleaseSRWLockExclusive(&g_lock);
    }

    static void __stdcall OnDestroy(uint32_t obj) {
        AcquireSRWLockExclusive(&g_lock);
        for (int i = 0; i < g_count; ++i)
            if (g_slots[i].obj == obj) { g_slots[i].obj = 0; g_slots[i].ours = false; g_slots[i].spot = false; }
        ReleaseSRWLockExclusive(&g_lock);
    }

#define MARKER_STUB(Name, Fn, Orig) \
    static __declspec(naked) void Name() { \
        __asm pushfd \
        __asm pushad \
        __asm push ecx \
        __asm call Fn \
        __asm popad \
        __asm popfd \
        __asm jmp dword ptr [Orig] \
    }
    MARKER_STUB(CtorSingleStub, OnCreate, g_ctorSingle)
    MARKER_STUB(CtorDualStub, OnCreate, g_ctorDual)
    MARKER_STUB(DtorSingleStub, OnDestroy, g_dtorSingle)
    MARKER_STUB(DtorDualStub, OnDestroy, g_dtorDual)
#undef MARKER_STUB

    static bool IsMarker(uint32_t obj) {
        uint32_t vt = 0;
        return obj && ReadU32(obj, vt) && (vt == Addr32(kVtSingle) || vt == Addr32(kVtDual));
    }

    static bool ReadSuspended(uint32_t obj, bool& suspended) {
        uint32_t w = 0;
        if (!ReadU32(obj + (kSuspendedByte & ~3u), w)) return false;
        suspended = ((w >> ((kSuspendedByte & 3) * 8)) & 0xFF) != 0;
        return true;
    }

    static bool SetSuspended(uint32_t obj, bool on) {
        uint32_t r = 0;
        return TryCallAny1(Addr32(kSetSuspended), obj, on ? 1u : 0u, r);
    }

    static int Snapshot(uint32_t* out, bool wantOurs) {
        int n = 0;
        AcquireSRWLockExclusive(&g_lock);
        for (int i = 0; i < g_count && n < kMax; ++i)
            if (g_slots[i].obj && g_slots[i].ours == wantOurs) out[n++] = g_slots[i].obj;
        ReleaseSRWLockExclusive(&g_lock);
        return n;
    }

    static void MarkOurs(uint32_t obj, bool ours) {
        AcquireSRWLockExclusive(&g_lock);
        for (int i = 0; i < g_count; ++i) if (g_slots[i].obj == obj) g_slots[i].ours = ours;
        ReleaseSRWLockExclusive(&g_lock);
    }

    static uint32_t g_work[kMax * 2];

    static bool IsSpot(uint32_t obj) {
        bool r = false;
        AcquireSRWLockExclusive(&g_lock);
        for (int i = 0; i < g_count; ++i) if (g_slots[i].obj == obj) { r = g_slots[i].spot; break; }
        ReleaseSRWLockExclusive(&g_lock);
        return r;
    }

    static void MarkSpot(uint32_t obj, bool spot) {
        AcquireSRWLockExclusive(&g_lock);
        for (int i = 0; i < g_count; ++i) if (g_slots[i].obj == obj) g_slots[i].spot = spot;
        ReleaseSRWLockExclusive(&g_lock);
    }

    static int SnapshotAll(uint32_t* out) {
        int m = Snapshot(out, false);
        return m + Snapshot(out + m, true);
    }

    static int SuspendActive() {
        int n = 0, m = SnapshotAll(g_work);
        for (int i = 0; i < m; ++i) {
            bool susp = true;
            uint32_t o = g_work[i];
            if (!IsMarker(o) || !ReadSuspended(o, susp) || susp || IsSpot(o)) continue;
            if (SetSuspended(o, true)) { MarkOurs(o, true); ++n; }
        }
        return n;
    }

    int FlagSpotsTurnedOnBy(void (*fn)(void*), void* ctx) {
        static uint32_t before[kMax * 2];
        static uint8_t wasSusp[kMax * 2];
        int m = SnapshotAll(before);
        for (int i = 0; i < m; ++i) { bool su = true; wasSusp[i] = (IsMarker(before[i]) && ReadSuspended(before[i], su) && su) ? 1 : 0; }
        fn(ctx);
        int n = 0;
        for (int i = 0; i < m; ++i) {
            bool su = true;
            if (!wasSusp[i] || !IsMarker(before[i]) || !ReadSuspended(before[i], su) || su) continue;
            MarkSpot(before[i], true);
            MarkOurs(before[i], false);
            ++n;
        }
        return n;
    }

    void Hide() {
        g_hidden = true;
        int n = SuspendActive();
        LOG("[markers] %d in-world event marker(s) suspended (%d tracked)", n, g_count);
        (void)n;
    }

    void Tick() {
        if (!g_hidden) return;
        int n = SuspendActive();
        if (n) LOG("[markers] %d more marker(s) suspended", n);
        (void)n;
    }

    void Restore() {
        if (!g_hidden) return;
        g_hidden = false;
        int n = 0, hidden = 0, m = SnapshotAll(g_work);
        for (int i = 0; i < m; ++i) {
            uint32_t o = g_work[i];
            bool ours = false, spot = false;
            AcquireSRWLockExclusive(&g_lock);
            for (int k = 0; k < g_count; ++k) if (g_slots[k].obj == o) { ours = g_slots[k].ours; spot = g_slots[k].spot; g_slots[k].ours = g_slots[k].spot = false; break; }
            ReleaseSRWLockExclusive(&g_lock);
            bool susp = false;
            if (!IsMarker(o) || !ReadSuspended(o, susp)) continue;
            if (spot) { if (!susp && SetSuspended(o, true)) ++hidden; }
            else if (ours && susp && SetSuspended(o, false)) ++n;
        }
        LOG("[markers] %d in-world event marker(s) resumed, %d hiding spot marker(s) removed", n, hidden);
        (void)n; (void)hidden;
    }

    void Install() {
        g_ctorSingle = Addr32(kCtorSingle);
        Hooks::Attach(reinterpret_cast<void**>(&g_ctorSingle), reinterpret_cast<void*>(CtorSingleStub), "event marker single ctor");
        g_ctorDual = Addr32(kCtorDual);
        Hooks::Attach(reinterpret_cast<void**>(&g_ctorDual), reinterpret_cast<void*>(CtorDualStub), "event marker dual ctor");
        g_dtorSingle = Addr32(kDtorSingle);
        Hooks::Attach(reinterpret_cast<void**>(&g_dtorSingle), reinterpret_cast<void*>(DtorSingleStub), "event marker single dtor");
        g_dtorDual = Addr32(kDtorDual);
        Hooks::Attach(reinterpret_cast<void**>(&g_dtorDual), reinterpret_cast<void*>(DtorDualStub), "event marker dual dtor");
    }

    void Remove() {
        Hooks::Detach(reinterpret_cast<void**>(&g_ctorSingle), reinterpret_cast<void*>(CtorSingleStub));
        Hooks::Detach(reinterpret_cast<void**>(&g_ctorDual), reinterpret_cast<void*>(CtorDualStub));
        Hooks::Detach(reinterpret_cast<void**>(&g_dtorSingle), reinterpret_cast<void*>(DtorSingleStub));
        Hooks::Detach(reinterpret_cast<void**>(&g_dtorDual), reinterpret_cast<void*>(DtorDualStub));
    }
}
