#pragma once

// Background thread (50ms): F9 arms/disarms the mod; while armed, a hit on a
// cop requests a pursuit (Pursuit::RequestHit, executed on the game thread).
// Hit = nearest cop closer than 5.0 AND (player speed drop > 4.0 in one tick,
// OR velocity direction change > 30deg above speed 3, OR a collision
// activation within the last 200ms). 3s cooldown; re-armed when the cop is
// farther than 7.5.
namespace Mod::Trigger {
    void Start();
    void Stop(bool wait);
}
