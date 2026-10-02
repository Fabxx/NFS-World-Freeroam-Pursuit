#pragma once
#include <windows.h>
#include <cstdint>
namespace Mod::Stats {
    struct Snapshot { uint32_t copsDeployed, copsDisabled, copsRammed, costToState; bool valid; };
    void Install();
    void Remove();
    void Reset();
    Snapshot Get();
}
