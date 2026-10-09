// NFSWorldPursuitProbe: with Options > Gameplay > Freeroam Pursuits ON, ramming a cop in freeroam starts a pursuit like event 385.
#include <windows.h>
#include "Hooks.h"
#include "Music.h"
#include "Pursuit.h"
#include "Results.h"
#include "Stats.h"
#include "Guard.h"
#include "SpotFx.h"
#include "MarkerFx.h"
#include "HeatTimer.h"
#include "Trigger.h"
#include "Options.h"
#include "Log.h"

static bool g_running = false;

static DWORD WINAPI InitThread(LPVOID) {
    Mod::Log::Open();
    Mod::Log::InstallCrashLogger();
#ifdef _DEBUG
    LOG("NFSWorldPursuitProbe (debug build) loaded");
#else
    LOG("NFSWorldPursuitProbe (release build) loaded");
#endif
    Mod::Hooks::InstallCore();
    Mod::Pursuit::Install();
    Mod::Music::Install();
    Mod::Results::Install();
    Mod::Stats::Install();
    Mod::Guard::Install();
    Mod::SpotFx::Install();
    Mod::MarkerFx::Install();
    Mod::HeatTimer::Install();
    Mod::Options::Install();
    Mod::Trigger::Start();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        // A second copy of the .asi in the same game (e.g. Debug and Release builds) would hook everything twice.
        char name[64];
        wsprintfA(name, "NFSWorldPursuitProbe_%lu", GetCurrentProcessId());
        HANDLE once = CreateMutexA(nullptr, FALSE, name);
        if (!once || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (once) CloseHandle(once);
            return TRUE;
        }
        g_running = true;
        if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        if (!g_running) return TRUE;
        if (reserved == nullptr) {
            Mod::Trigger::Stop(true);
            Mod::Options::Remove();
            Mod::HeatTimer::Remove();
            Mod::MarkerFx::Remove();
            Mod::SpotFx::Remove();
            Mod::Guard::Remove();
            Mod::Stats::Remove();
            Mod::Results::Remove();
            Mod::Music::Remove();
            Mod::Pursuit::Remove();
            Mod::Hooks::RemoveCore();
        }
        Mod::Log::RemoveCrashLogger();
        Mod::Log::Close();
    }
    return TRUE;
}
