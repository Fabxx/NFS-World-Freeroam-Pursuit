#pragma once
#include <cstdint>

namespace Probe {
    // Calls the client-side single-player event launcher (same call the
    // World Map's "launch event" button makes) with the given eventId.
    // flag=1 is the only confirmed-working value; flag=0 is confirmed
    // BAD (soft-locks the launcher until game restart) -- never use it.
    bool TryStartSinglePlayerEvent(uintptr_t moduleBase, uint32_t eventId, int flag);
}
