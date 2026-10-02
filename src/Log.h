#pragma once
#include <cstdint>

namespace Mod::Log {
    bool Enabled();
    void Open();
    void Close();
    void Write(const char* fmt, ...);
    void InstallCrashLogger();
    void RemoveCrashLogger();
#ifdef _DEBUG
    void LaunchTestEvent();
#endif
}
#define LOG(...) do { if (::Mod::Log::Enabled()) ::Mod::Log::Write(__VA_ARGS__); } while (0)
