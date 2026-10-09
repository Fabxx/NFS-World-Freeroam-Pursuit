#pragma once
#include <windows.h>

namespace Mod::Results {
    void Install();
    void Remove();
    void Tick(ULONGLONG now);
    void Commit();
    void PursuitStarted();
    void ChaseOver();
    // REP della sola parte "inseguimento" guadagnata finora (stessa formula della schermata dei risultati).
    int PursuitRepSoFar();
}
