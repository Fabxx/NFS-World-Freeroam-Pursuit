// NFS World: ramming a cop in freeroam starts a real pursuit (F9 toggles the mod).
#include <windows.h>
#include "Hooks.h"
#include "Music.h"
#include "Pursuit.h"
#include "Results.h"
#include "MarkerFx.h"
#include "Trigger.h"
#include "Log.h"

// Hooks are attached off the loader lock.
static DWORD WINAPI InitThread(LPVOID) {
#ifdef _DEBUG
    Mod::Log::Open();
    Mod::Log::InstallCrashLogger();
    LOG("NFSWorldPursuitProbe (debug build) loaded");
#endif
    Mod::Hooks::InstallCore();
    Mod::Pursuit::Install();
    Mod::Music::Install();
    Mod::Results::Install();
    Mod::MarkerFx::Install();
#ifdef _DEBUG
    Mod::Log::InstallMapProbe();
    Mod::Log::InstallScriptProbe();
#endif
    Mod::Trigger::Start();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        if (reserved == nullptr) {   // FreeLibrary: undo everything. Process exit: nothing to undo.
            Mod::Trigger::Stop(true);
#ifdef _DEBUG
            Mod::Log::RemoveScriptProbe();
            Mod::Log::RemoveMapProbe();
#endif
            Mod::MarkerFx::Remove();
            Mod::Results::Remove();
            Mod::Music::Remove();
            Mod::Pursuit::Remove();
            Mod::Hooks::RemoveCore();
        }
#ifdef _DEBUG
        Mod::Log::RemoveCrashLogger();
        Mod::Log::Close();
#endif
    }
    return TRUE;
}
