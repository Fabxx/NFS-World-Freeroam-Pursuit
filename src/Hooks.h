#pragma once
#include <windows.h>
#include <cstdint>

namespace Mod::Hooks {
    bool Attach(void** original, void* detour, const char* name);
    void Detach(void** original, void* detour);

    void InstallCore();
    void RemoveCore();

    ULONGLONG LastCollisionMs();
    uint32_t ExitPursuitModeCalls();
}
