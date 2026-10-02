// Worker thread: cop-hit detection (armed by Options > Gameplay > Freeroam Pursuits).
#include "Trigger.h"
#include "Game.h"
#include "Hooks.h"
#include "Pursuit.h"
#include "Log.h"
#include "Options.h"
#include <atomic>
#include <cmath>

namespace Mod::Trigger {
    constexpr DWORD     kPollMs            = 50;
    constexpr float     kHitRadius         = 5.0f;
    constexpr float     kReleaseRadius     = kHitRadius * 1.5f;
    constexpr float     kMinSpeedDrop      = 4.0f;
    constexpr float     kMinAngleChangeDeg = 30.0f;
    constexpr float     kMinSpeedForAngle  = 3.0f;
    constexpr ULONGLONG kCollisionWindowMs = 200;
    constexpr ULONGLONG kCooldownMs        = 3000;
    constexpr int       kNearestCopVt      = 44;
    constexpr int       kVelocityVt        = 8;

    static HANDLE g_thread = nullptr;
    static std::atomic<bool> g_stop{ false };

    static bool NearestCopDistance(float& dist) {
        uint32_t mgr = 0, vt = 0, fn = 0;
        if (!ReadU32(Addr(Ida::CopMgrPtr), mgr) || !mgr || !ReadU32(mgr, vt) || !ReadU32(vt + kNearestCopVt, fn) || !fn)
            return false;
        float angle = 0.0f, d = 0.0f;
        __try {
            reinterpret_cast<void(__thiscall*)(void*, float*, float*)>(fn)(reinterpret_cast<void*>(mgr), &angle, &d);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        dist = d;
        return true;
    }

    static bool PlayerVelocity(float v[3]) {
        uint32_t count = 0, base = 0, player = 0, table = 0, body = 0, vt = 0, fn = 0, vec = 0;
        if (!ReadU32(Addr(Ida::LocalPlayerCount), count) || !count || !ReadU32(Addr(Ida::LocalPlayerBase), base) || !base ||
            !ReadU32(base, player) || !player || !ReadU32(player + 4, table) || !table)
            return false;
        __try {
            body = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(reinterpret_cast<void*(__thiscall*)(void*, void*)>(
                Addr(Ida::QueryInterface))(reinterpret_cast<void*>(table), reinterpret_cast<void*>(Addr(Ida::KeyICollisionBody)))));
            if (!body || !ReadU32(body, vt) || !ReadU32(vt + kVelocityVt, fn) || !fn) return false;
            vec = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
                reinterpret_cast<void*(__thiscall*)(void*)>(fn)(reinterpret_cast<void*>(body))));
            if (!vec) return false;
            for (int i = 0; i < 3; ++i) v[i] = reinterpret_cast<volatile float*>(vec)[i];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static DWORD WINAPI ThreadProc(LPVOID) {
        bool latched = false, haveLast = false;
#ifdef _DEBUG
        bool wasF7 = false;
#endif
        float last[3] = {};
        ULONGLONG lastSampleMs = 0, lastHitMs = 0;

        while (!g_stop.load(std::memory_order_relaxed)) {
            Sleep(kPollMs);
            ResolveGameThread();

#ifdef _DEBUG
            bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
            if (f7 && !wasF7 && GameHasFocus()) Log::LaunchTestEvent();
            wasF7 = f7;
#endif
            if (!Options::Armed()) { haveLast = false; latched = false; continue; }

            ULONGLONG now = GetTickCount64();
            float dist = 0.0f;
            if (!NearestCopDistance(dist)) continue;
            if (dist <= 0.0f) { latched = false; continue; }

            float v[3];
            bool jolt = false;
            if (PlayerVelocity(v)) {
                float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (haveLast && now > lastSampleMs && now - lastSampleMs < 1000) {
                    float lastSpeed = std::sqrt(last[0] * last[0] + last[1] * last[1] + last[2] * last[2]);
                    float angle = 0.0f;
                    if (lastSpeed > kMinSpeedForAngle && speed > kMinSpeedForAngle) {
                        float dot = (last[0] * v[0] + last[1] * v[1] + last[2] * v[2]) / (lastSpeed * speed);
                        angle = std::acos(std::fmax(-1.0f, std::fmin(1.0f, dot))) * 57.295776f;
                    }
                    jolt = (lastSpeed - speed > kMinSpeedDrop) || angle > kMinAngleChangeDeg;
                }
                last[0] = v[0]; last[1] = v[1]; last[2] = v[2];
                lastSampleMs = now;
                haveLast = true;
            }
            else {
                haveLast = false;
            }

            ULONGLONG collisionMs = Hooks::LastCollisionMs();
            bool contact = collisionMs && now >= collisionMs && now - collisionMs <= kCollisionWindowMs;
            bool cooldownOver = !lastHitMs || now - lastHitMs >= kCooldownMs;

            if (!latched && cooldownOver && dist < kHitRadius && (jolt || contact)) {
                LOG("[trigger] cop hit: dist %.2f jolt %d contact %d", dist, jolt ? 1 : 0, contact ? 1 : 0);
                Pursuit::RequestHit();
                latched = true;
                lastHitMs = now;
            }
            else if (latched && dist > kReleaseRadius) {
                latched = false;
            }
        }
        return 0;
    }

    void Start() {
        if (g_thread) return;
        g_stop.store(false);
        g_thread = CreateThread(nullptr, 0, ThreadProc, nullptr, 0, nullptr);
    }

    void Stop(bool wait) {
        if (!g_thread) return;
        g_stop.store(true);
        if (wait) WaitForSingleObject(g_thread, 1000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}
