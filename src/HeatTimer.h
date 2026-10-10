#pragma once
#include <windows.h>
#include <cstdint>

namespace Mod::HeatTimer {
    void Install();
    void Remove();
    // Chiamato dal Watch del pursuit (thread di gioco) circa ogni 200 ms con il calore attuale del giocatore.
    // paused = raffreddamento (il timer del livello si ferma).
    // Restituisce il calore da imporre al giocatore (>= 0) oppure -1 se non serve cambiarlo.
    // Calore salvato per il prossimo inseguimento (es. arresto -> livello minimo).
    void ResetSaved(float heat);
    float Tick(ULONGLONG now, bool active, float heat, bool paused = false);
}
