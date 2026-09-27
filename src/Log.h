#pragma once
#include <cstdint>
// Logging and diagnostics exist only in Debug builds. In Release LOG() expands
// to nothing (its arguments are not evaluated) and no file is created.
// Debug log: NFSWorldPursuitProbe.log next to the .asi.

#ifdef _DEBUG
namespace Mod::Log {
    void Open();
    void Close();
    void Write(const char* fmt, ...);
    // Vectored handler logging crashes (IDA address + nfsw.exe return
    // addresses on the stack) while our pursuit / results screen is active.
    void InstallCrashLogger();
    void RemoveCrashLogger();
    // [map] lines: map icon selectors / pursuit-breaker POI calls (MapProbe.cpp).
    void InstallMapProbe();
    void RemoveMapProbe();
    void InstallScriptProbe();   // [native]: first call of every EA# script native
    void RemoveScriptProbe();
    void ArmNativeCapture(uint32_t ms);   // [cap]: every native call (deduplicated) for ms
    void PollMapLayers();   // [layers]: map layer visibility table, logged on change (game thread)
    // F7 (DebugEvent.cpp): launch the real pursuit event 385 for comparisons.
    void LaunchTestEvent();
}
#define LOG(...) ::Mod::Log::Write(__VA_ARGS__)
#else
#define LOG(...) ((void)0)
#endif
