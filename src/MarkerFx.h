#pragma once

// In-world event markers (light column + hologram at every event start).
// Each is an "activity" behavior of the event: single effect (ctor sub_6882F0,
// vt 0xBB8D94, dtor sub_694F90) or dual effect (ctor sub_688400, vt 0xBB8DC4,
// dtor sub_6950E0). Activity byte +37 = suspended, changed only through
// sub_774CA0(this, v): 1 -> vt[8] removes the effect, 0 -> vt[9] spawns it.
// A real event suspends the active ones at launch and resumes them after.
namespace Mod::MarkerFx {
    void Install();
    void Remove();
    void Hide();      // game thread: suspend every active marker (and keep doing it via Tick)
    void Tick();      // game thread: while hidden, suspend markers created meanwhile
    void Restore();   // game thread: resume the markers we suspended
}
