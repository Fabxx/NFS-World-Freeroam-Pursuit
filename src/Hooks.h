#pragma once
#include <windows.h>
#include <cstdint>

namespace Mod::Hooks {
    // Single-hook Detours helpers. *original holds the target on entry and the
    // trampoline afterwards (nulled on failure).
    bool Attach(void** original, void* detour, const char* name);
    void Detach(void** original, void* detour);

    // Hooks every feature relies on:
    //  - QueryInterface (sub_7536B0): called every frame on the game thread,
    //    used as the pump for all our game calls (Pursuit::PumpGameThread).
    //  - CSTATE_Collision activate (sub_812600): timestamp of the last contact.
    //  - EXIT-PURSUIT-MODE (sub_438A50): the game calls it when the results
    //    screen is closed -> we leave the pursuit.
    void InstallCore();
    void RemoveCore();

    ULONGLONG LastCollisionMs();
    uint32_t ExitPursuitModeCalls();
}
