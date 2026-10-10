#pragma once
#include <windows.h>
#include "Stats.h"

namespace Mod::Results {
    void Install();
    void Remove();
    void Tick(ULONGLONG now);
    void Commit();
    void PursuitStarted();
    void ChaseOver();
    // REP della sola parte "inseguimento" guadagnata finora (stessa formula della schermata dei risultati).
    int PursuitRepSoFar();
    // REP "inseguimento" di un evento (team escape) calcolata dalle statistiche dell'HUD dell'evento.
    int EventRepSoFar(const ::Mod::Stats::Snapshot& hs);
}
