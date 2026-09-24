#pragma once

// Minimal, dependency-free logger: a plain text log file next to the
// game exe (right next to this DLL's own .asi file). Deliberately does
// NOT pull in spdlog/fmt/etc so this whole project builds with nothing
// but the Windows SDK -- no vcpkg, no External\ folder to set up.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>

namespace Probe {

    class Logger {
    public:
        static Logger& Instance() {
            static Logger instance;
            return instance;
        }

        // Opens the log file at an ABSOLUTE path next to THIS DLL itself
        // (i.e. right next to the .asi file, in the game's plugin
        // folder), resolved via this module's own handle -- that
        // location is always the same regardless of the game's working
        // directory, and is always writable (the game just loaded the
        // .asi from there). Falls back to %TEMP% if that somehow still
        // fails (e.g. the plugin folder is read-only).
        void InitConsole() {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_logFile) return; // already initialized

            std::string path;

            HMODULE ownModule = nullptr;
            if (GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(&Logger::Instance),
                    &ownModule) && ownModule) {
                char modulePath[MAX_PATH] = {};
                DWORD len = GetModuleFileNameA(ownModule, modulePath, MAX_PATH);
                if (len > 0 && len < MAX_PATH) {
                    std::string full(modulePath);
                    size_t slash = full.find_last_of("\\/");
                    if (slash != std::string::npos) {
                        path = full.substr(0, slash + 1) + "NFSWorldPursuitProbe.log";
                    }
                }
            }

            if (!path.empty()) {
                fopen_s(&_logFile, path.c_str(), "w");
            }

            if (!_logFile) {
                // Own-folder attempt failed (module lookup failed, or the
                // plugin folder isn't writable) -- fall back to a location
                // that's essentially always writable.
                char tempDir[MAX_PATH] = {};
                DWORD tlen = GetTempPathA(MAX_PATH, tempDir);
                if (tlen > 0 && tlen < MAX_PATH) {
                    std::string candidate = std::string(tempDir) + "NFSWorldPursuitProbe.log";
                    fopen_s(&_logFile, candidate.c_str(), "w");
                    if (_logFile) {
                        path = candidate;
                    }
                }
            }

            if (_logFile) {
                _logFilePath = path;
                fprintf(_logFile, "[log] Log file opened at: %s\n", path.c_str());
                fflush(_logFile);
            }
        }

        std::string GetLogFilePath() {
            std::lock_guard<std::mutex> lock(_mutex);
            return _logFilePath;
        }

        void Shutdown() {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_logFile) {
                fclose(_logFile);
                _logFile = nullptr;
            }
        }

        template <typename... Args>
        void Info(const char* fmt, Args... args) {
            Write("INFO", fmt, args...);
        }

        template <typename... Args>
        void Warn(const char* fmt, Args... args) {
            Write("WARN", fmt, args...);
        }

    private:
        Logger() = default;
        ~Logger() { Shutdown(); }
        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

        template <typename... Args>
        void Write(const char* level, const char* fmt, Args... args) {
            std::lock_guard<std::mutex> lock(_mutex);

            SYSTEMTIME st;
            GetLocalTime(&st);
            char prefix[64];
            snprintf(prefix, sizeof(prefix), "[%02u:%02u:%02u.%03u] [%s] ",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level);

            char body[1024];
            snprintf(body, sizeof(body), fmt, args...);

            if (_logFile) {
                fprintf(_logFile, "%s%s\n", prefix, body);
                fflush(_logFile);
            }
        }

        FILE* _logFile = nullptr;
        std::string _logFilePath;
        std::mutex _mutex;
    };

}
