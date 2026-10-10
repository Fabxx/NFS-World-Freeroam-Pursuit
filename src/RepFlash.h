#pragma once
#include <windows.h>
#include <cstdint>
#include "Stats.h"

namespace Mod::RepFlash {
    void Install();
    void Remove();
    // Chiamato dal thread di gioco a ogni aggiornamento dell'HUD dell'inseguimento.
    void Poll();
    // Inseguimento di un evento (team escape): REP calcolata dalle statistiche dell'HUD dell'evento.
    void PollEvent(const ::Mod::Stats::Snapshot& hs, uint32_t session);
}
