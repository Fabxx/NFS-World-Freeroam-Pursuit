#pragma once
#include <windows.h>
#include <cstdint>

namespace Mod::Pursuit {
    void Install();   // mode-getter hook + optional config file
    void Remove();

    void RequestHit();        // trigger thread: a cop was rammed
    void PumpGameThread();    // QueryInterface hook, every frame (game thread only)

    // True while our pursuit, its results screen, or the 30s after the exit
    // are running: the results screen is then answered by Results.
    bool InResultsWindow();
    bool IsActive();          // our pursuit or its results screen only

    struct Stats { ULONGLONG startMs; ULONGLONG endMs; uint32_t maxCops; };
    Stats GetStats();
}
