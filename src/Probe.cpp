#include "Probe.h"
#include "Logger.h"
#include "CopProximityTriggerHook.h"

#include <windows.h>
#include <cstdint>
#include <atomic>

namespace Probe {

    static HANDLE g_thread = nullptr;
    static std::atomic<bool> g_running{ false };
    static uintptr_t g_moduleBase = 0;

    static DWORD WINAPI ThreadMain(LPVOID) {
        auto& log = Logger::Instance();

        g_moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(NULL));
        log.Info("[probe] Started. Module base = 0x%08X.", (unsigned)g_moduleBase);

        while (g_running.load()) {
            PollCopProximityTrigger(g_moduleBase);
            Sleep(50);
        }

        return 0;
    }

    void Start() {
        if (g_running.load()) return;
        g_running.store(true);
        g_thread = CreateThread(NULL, 0, ThreadMain, NULL, 0, NULL);
    }

    void Stop() {
        g_running.store(false);
        if (g_thread) {
            WaitForSingleObject(g_thread, 2000);
            CloseHandle(g_thread);
            g_thread = nullptr;
        }
        Logger::Instance().Shutdown();
    }

}
