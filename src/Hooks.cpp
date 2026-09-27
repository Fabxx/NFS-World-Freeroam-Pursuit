#include "Hooks.h"
#include "Game.h"
#include "Pursuit.h"
#include "Log.h"
#include <detours.h>
#include <atomic>

namespace Mod::Hooks {

    bool Attach(void** original, void* detour, const char* name) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        LONG e1 = DetourAttach(original, detour);
        LONG e2 = (e1 == NO_ERROR) ? DetourTransactionCommit() : (DetourTransactionAbort(), e1);
        if (e1 != NO_ERROR || e2 != NO_ERROR) {
            LOG("[hooks] %s: attach FAILED (%ld/%ld)", name, e1, e2);
            *original = nullptr;
            return false;
        }
        LOG("[hooks] %s attached", name);
        (void)name;
        return true;
    }

    void Detach(void** original, void* detour) {
        if (!*original) return;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(original, detour);
        DetourTransactionCommit();
        *original = nullptr;
    }

    using QueryInterface_t = int(__thiscall*)(void*, int);
    using CollisionActivate_t = int(__thiscall*)(void*, void*);
    using ExitPursuitMode_t = int(__thiscall*)(void*);

    static QueryInterface_t g_origQI = nullptr;
    static CollisionActivate_t g_origCollision = nullptr;
    static ExitPursuitMode_t g_origExit = nullptr;
    static std::atomic<ULONGLONG> g_lastCollisionMs{ 0 };
    static std::atomic<uint32_t> g_exitCalls{ 0 };

    static int __fastcall QueryInterfaceHook(void* self, void*, int key) {
        Pursuit::PumpGameThread();
        return g_origQI(self, key);
    }

    static int __fastcall CollisionActivateHook(void* self, void*, void* a2) {
        g_lastCollisionMs.store(GetTickCount64(), std::memory_order_relaxed);
        return g_origCollision(self, a2);
    }

    static int __fastcall ExitPursuitModeHook(void* self, void*) {
        g_exitCalls.fetch_add(1, std::memory_order_acq_rel);
        return g_origExit(self);
    }

    void InstallCore() {
        g_origQI = reinterpret_cast<QueryInterface_t>(Addr(Ida::QueryInterface));
        g_origCollision = reinterpret_cast<CollisionActivate_t>(Addr(Ida::CollisionActivate));
        g_origExit = reinterpret_cast<ExitPursuitMode_t>(Addr(Ida::ExitPursuitMode));
        Attach(reinterpret_cast<void**>(&g_origQI), reinterpret_cast<void*>(QueryInterfaceHook), "QueryInterface pump");
        Attach(reinterpret_cast<void**>(&g_origCollision), reinterpret_cast<void*>(CollisionActivateHook), "collision activate");
        Attach(reinterpret_cast<void**>(&g_origExit), reinterpret_cast<void*>(ExitPursuitModeHook), "EXIT-PURSUIT-MODE");
    }

    void RemoveCore() {
        Detach(reinterpret_cast<void**>(&g_origExit), reinterpret_cast<void*>(ExitPursuitModeHook));
        Detach(reinterpret_cast<void**>(&g_origCollision), reinterpret_cast<void*>(CollisionActivateHook));
        Detach(reinterpret_cast<void**>(&g_origQI), reinterpret_cast<void*>(QueryInterfaceHook));
    }

    ULONGLONG LastCollisionMs() { return g_lastCollisionMs.load(std::memory_order_relaxed); }
    uint32_t ExitPursuitModeCalls() { return g_exitCalls.load(std::memory_order_acquire); }
}
