#pragma once

// World map + minimap icons. Map manager: dword_D11EF4 -> +4 = obj
//   obj+24  std::map<layer id, layer*>; icon drawn only if layer->+36 != 0
//   obj+52  std::vector<entity*> (begin/end), one entity per icon
// entity: +24 visible flag (vt[11] sub_773610 sets it), vt[12] sub_6CE620 =
// entity+24 && layer->+36. Layer 1 = events, layer 7 = pursuit breakers
// (entity vt 0xBC2210, +100 == 1) and cooldown hiding spots (+100 == 0).
// Layer visibility = layer->+36 + view->vt[7](layer, show) on the views at
// obj+8 (UI: world map and icons over the 3D world) and obj+12 (minimap).
// A real pursuit event: layer 1 off at launch, PB entities on when the chase
// starts, hiding spots on at cooldown, everything back at the end.
namespace Mod::MapIcons {
    void EnterPursuit();   // hide event icons, show pursuit breakers
    void SetCooldown(bool on);     // breakers off + hiding spots on, or the reverse
    void HidePursuitIcons();       // chase over: every breaker / hiding spot we showed
    void ExitPursuit();    // restore everything we touched
}
