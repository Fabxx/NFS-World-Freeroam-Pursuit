#include <windows.h>
#include "Probe.h"
#include "Logger.h"
#include "CopProximityTriggerHook.h"

static DWORD WINAPI InitThread(LPVOID) {
    Probe::Start();
    Probe::SetupCopProximityTrigger();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*lpReserved*/) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hModule);
        // Open the log file synchronously, before any thread can log to it.
        Probe::Logger::Instance().InitConsole();
        HANDLE hThread = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
        if (hThread) CloseHandle(hThread);
        break;
    }
    case DLL_PROCESS_DETACH:
        Probe::CleanupCopProximityTrigger();
        Probe::Stop();
        break;
    }
    return TRUE;
}
