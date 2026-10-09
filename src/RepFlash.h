#pragma once
#include <windows.h>

namespace Mod::RepFlash {
    void Install();
    void Remove();
    // Chiamato dal thread di gioco a ogni aggiornamento dell'HUD dell'inseguimento.
    void Poll();
}
