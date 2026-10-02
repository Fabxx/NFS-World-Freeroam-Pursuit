#pragma once
#include <windows.h>

namespace Mod::Results {
    void Install();
    void Remove();
    void Tick(ULONGLONG now);
    void Commit();
    void PursuitStarted();
    void ChaseOver();
}
