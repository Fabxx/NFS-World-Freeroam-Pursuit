#pragma once
#include <windows.h>
#include <cstdint>

namespace Mod::Music {
    void Install();
    void Remove();
    void BeginPulse(ULONGLONG now);
    void Tick(ULONGLONG now);
    void ApplyPursuit(uint32_t eventKey);
    void Restore();
}
