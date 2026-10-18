#pragma once
#include <windows.h>
#include <string>

// Auto in uso letta dai file del server offline (drivers\driver*\carslots.xml): serve a tenere il calore
// salvato per ogni auto, invece di un unico valore per tutte.
namespace Mod::ServerCar {
    struct Info {
        bool ok = false;
        std::string id;        // OwnedCarTrans/Id dell'auto in uso (DefaultOwnedCarIndex)
        std::string name;      // CustomCar/Name, solo per il registro
        float heat = -1.0f;    // OwnedCarTrans/Heat salvato dal server (-1 se assente)
    };
    // Legge l'auto in uso del pilota attivo. La cartella del server viene cercata una volta (sottocartelle della
    // cartella del gioco con drivers\DefaultPersonaIdx.xml e data\, preferendo quella il cui config\config.json
    // punta a questo nfsw.exe) oppure letta da NFSWorldPursuitProbe_serverdir.txt.
    Info Current();
}
