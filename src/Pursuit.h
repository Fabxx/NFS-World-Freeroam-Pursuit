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
    uint32_t RoadblocksDodged();
    uint32_t SpikeStripsDodged();
    uint32_t Infractions();

    // topSpeed in m/s (come il campo TopSpeed del pacchetto dei risultati), infractions = tipi di infrazione
    // commessi durante l'inseguimento (bit della maschera del gioco), spikeStripsDodged = posti di blocco con
    // strisce chiodate superati senza toccarli.
    struct Stats { ULONGLONG startMs; ULONGLONG endMs; uint32_t maxCops; bool busted; float heat;
                   float topSpeed; uint32_t infractions; uint32_t spikeStripsDodged; };
    Stats GetStats();
}
