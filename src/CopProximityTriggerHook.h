#pragma once
#include <cstdint>

namespace Probe {
    // Polls distance to the nearest cop (ICopMgr) and the local player's
    // own vehicle velocity (ICollisionBody) every tick. When the player
    // is within kTriggerRadius of a cop AND a jolt is detected on the
    // player's own velocity (sudden speed drop or direction change),
    // launches TryStartSinglePlayerEvent(moduleBase, 385, 1) -- the
    // Pursuit event. Always armed, no key toggles.
    void SetupCopProximityTrigger();
    void CleanupCopProximityTrigger();

    // Call every tick from Probe's poll loop.
    void PollCopProximityTrigger(uintptr_t moduleBase);
}
