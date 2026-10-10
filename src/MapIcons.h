#pragma once

namespace Mod::MapIcons {
    void EnterPursuit();
    void SetCooldown(bool on);
    void HidePursuitIcons();
    void ExitPursuit();
    int CountHidingSpots();
    int CountBreakers();
    int HideTreasure();      // treasure hunt gems/areas on the map; returns how many were hidden
    int RestoreTreasure();   // shows again the ones HideTreasure hid
}
