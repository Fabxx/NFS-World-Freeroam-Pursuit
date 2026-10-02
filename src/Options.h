#pragma once
#include <windows.h>

// "Freeroam pursuits" switch: Options > Gameplay (GamePlayOptions.moments).
namespace Mod::Options {
    void Install();
    void Remove();
    void Tick(ULONGLONG now);
    bool Armed();
}
