#pragma once

namespace Probe {
    // Starts the background polling thread. Safe to call once from
    // DllMain(DLL_PROCESS_ATTACH) -- returns immediately, the actual
    // polling runs on its own thread.
    void Start();

    // Stops the polling thread and joins it. Call from
    // DllMain(DLL_PROCESS_DETACH).
    void Stop();
}
