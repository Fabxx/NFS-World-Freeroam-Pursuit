#pragma once
#include <windows.h>
#include <cstdint>

// Pursuit music without the scripted event. Four things the event load does
// that our pursuit has to reproduce:
//  1. mask pulse: bit 20 of the music mask ON ~400ms, then OFF, before
//     LaunchPursuit -- makes the player drop the freeroam source;
//  2. snd Attrib instance (*(*dword_D77CC4+200)) pointed at the event's
//     collection: words {coll, *(coll+28), keep 2..5, freeroam[0], freeroam[1]},
//     coll = sub_6C1D10(40595996, event key);
//  3. forced song id dword_C850CC = 1 (the real event always plays 0/1);
//  4. music source byte +64 = 0 (source = *(player+52)), otherwise the player
//     never switches to the interactive pursuit source.
// All of it is restored when the pursuit ends.
namespace Mod::Music {
    void Install();                        // hook: learns the music player (ECX of sub_83BA90)
    void Remove();
    void BeginPulse(ULONGLONG now);
    void Tick(ULONGLONG now);              // ends the pulse
    void ApplyPursuit(uint32_t eventKey);  // 2..4, right before LaunchPursuit
    void Restore();
}
