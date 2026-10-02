#pragma once
#include <windows.h>
#include <cstdint>

namespace Mod::Pursuit {
    void Install();
    void Remove();

    void RequestHit();
    void PumpGameThread();

    bool InResultsWindow();
    bool IsActive();

    struct Stats { ULONGLONG startMs; ULONGLONG endMs; uint32_t maxCops; bool busted; float heat; };
    Stats GetStats();
}
