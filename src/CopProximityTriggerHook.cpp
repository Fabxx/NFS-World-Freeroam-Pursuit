#define NOMINMAX
#include "CopProximityTriggerHook.h"
#include "Logger.h"
#include "VtableScan.h"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cmath>

namespace Probe {

    namespace CopProximityOffsets {
        // ICopMgr* global (one level of indirection).
        constexpr uintptr_t COP_MGR_SINGLETON_PTR = 0xD1EEE0 - 0x400000;
        constexpr int kNearestCopQueryVtableOffset = 44; // slot 11, "nearest cop dist+bearing"

        // Local player object array: entry 0 is always the local player.
        constexpr uintptr_t LOCAL_PLAYER_ARRAY_COUNT = 0xD1FA90 - 0x400000;
        constexpr uintptr_t LOCAL_PLAYER_ARRAY_BASE = 0xD1FA88 - 0x400000;
        constexpr uintptr_t QUERY_INTERFACE_FN = 0x7536B0 - 0x400000;
        constexpr uintptr_t ICOLLISIONBODY_KEY_FN = 0x6519F0 - 0x400000;
        constexpr int kVelocityGetterVtableOffset = 8; // ICollisionBody slot 2, {vx,vy,vz}
    }

    constexpr uint32_t kPursuitEventId = 385; // pursuitoutrun_3
    constexpr float kTriggerRadius = 5.0f;
    constexpr float kJoltMinSpeedDrop = 4.0f;        // units/sec dropped in one tick
    constexpr float kJoltMinAngleChangeDeg = 30.0f;  // velocity direction change
    constexpr float kJoltMinSpeedForAngle = 3.0f;    // ignore angle noise below this speed
    constexpr DWORD kCooldownMs = 3000;

    static bool g_triggerLatched = false;
    static float g_lastDist = -1.0f;
    static ULONGLONG g_lastSampleTimeMs = 0;
    static ULONGLONG g_lastTriggerTimeMs = 0;
    static bool g_haveLastVelocity = false;
    static float g_lastVelX = 0.0f, g_lastVelY = 0.0f, g_lastVelZ = 0.0f;
    static ULONGLONG g_lastVelSampleTimeMs = 0;

    static bool SafeReadDword(uintptr_t address, uint32_t& outValue) {
        __try {
            outValue = *reinterpret_cast<volatile uint32_t*>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Local player array -> entry 0 -> QueryInterface(ICollisionBody) ->
    // velocity getter. SEH-guarded; returns false on any unreadable link.
    static bool TryGetLocalPlayerVelocity(uintptr_t moduleBase, float& outVx, float& outVy, float& outVz) {
        uint32_t count = 0;
        if (!SafeReadDword(moduleBase + CopProximityOffsets::LOCAL_PLAYER_ARRAY_COUNT, count) || count == 0) {
            return false;
        }

        uint32_t arrayBase = 0;
        if (!SafeReadDword(moduleBase + CopProximityOffsets::LOCAL_PLAYER_ARRAY_BASE, arrayBase) || arrayBase == 0) {
            return false;
        }

        uint32_t entry0 = 0;
        if (!SafeReadDword(static_cast<uintptr_t>(arrayBase), entry0) || entry0 == 0) {
            return false;
        }

        uint32_t ifaceTable = 0;
        if (!SafeReadDword(static_cast<uintptr_t>(entry0) + 4, ifaceTable) || ifaceTable == 0) {
            return false;
        }

        uint32_t collisionBodyPtr = 0;
        __try {
            using QueryInterface_t = void* (__thiscall*)(void*, void*);
            QueryInterface_t qi = reinterpret_cast<QueryInterface_t>(moduleBase + CopProximityOffsets::QUERY_INTERFACE_FN);
            void* key = reinterpret_cast<void*>(moduleBase + CopProximityOffsets::ICOLLISIONBODY_KEY_FN);
            collisionBodyPtr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
                qi(reinterpret_cast<void*>(static_cast<uintptr_t>(ifaceTable)), key)));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        if (collisionBodyPtr == 0) {
            return false;
        }

        uint32_t vtable = 0;
        if (!SafeReadDword(static_cast<uintptr_t>(collisionBodyPtr), vtable) || vtable == 0) {
            return false;
        }

        uint32_t fn = 0;
        if (!SafeReadDword(static_cast<uintptr_t>(vtable) + CopProximityOffsets::kVelocityGetterVtableOffset, fn) || fn == 0) {
            return false;
        }

        uint32_t velVecPtr = 0;
        __try {
            using VelocityGetter_t = void* (__thiscall*)(void*);
            VelocityGetter_t getVel = reinterpret_cast<VelocityGetter_t>(static_cast<uintptr_t>(fn));
            velVecPtr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
                getVel(reinterpret_cast<void*>(static_cast<uintptr_t>(collisionBodyPtr)))));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        if (velVecPtr == 0) {
            return false;
        }

        uint32_t rawX = 0, rawY = 0, rawZ = 0;
        if (!SafeReadDword(static_cast<uintptr_t>(velVecPtr) + 0, rawX)) return false;
        if (!SafeReadDword(static_cast<uintptr_t>(velVecPtr) + 4, rawY)) return false;
        if (!SafeReadDword(static_cast<uintptr_t>(velVecPtr) + 8, rawZ)) return false;

        float vx, vy, vz;
        memcpy(&vx, &rawX, sizeof(vx));
        memcpy(&vy, &rawY, sizeof(vy));
        memcpy(&vz, &rawZ, sizeof(vz));
        outVx = vx; outVy = vy; outVz = vz;
        return true;
    }

    // ICopMgr vtable slot 11 (+44): distance + bearing to the nearest cop,
    // the same call the native radar HUD uses. Read-only.
    static bool TryGetNearestCopDistance(uintptr_t moduleBase, float& outDistance, float& outBearingDeg) {
        uintptr_t globalAddr = moduleBase + CopProximityOffsets::COP_MGR_SINGLETON_PTR;

        uint32_t copMgrPtr = 0;
        if (!SafeReadDword(globalAddr, copMgrPtr) || copMgrPtr == 0) {
            return false;
        }

        uint32_t vtable = 0;
        if (!SafeReadDword(static_cast<uintptr_t>(copMgrPtr), vtable) || vtable == 0) {
            return false;
        }

        uint32_t fn = 0;
        if (!SafeReadDword(static_cast<uintptr_t>(vtable) + CopProximityOffsets::kNearestCopQueryVtableOffset, fn) || fn == 0) {
            return false;
        }

        float angleRad = 0.0f, dist = 0.0f;
        __try {
            using NearestCopQuery_t = void(__thiscall*)(void*, float*, float*);
            NearestCopQuery_t query = reinterpret_cast<NearestCopQuery_t>(static_cast<uintptr_t>(fn));
            query(reinterpret_cast<void*>(static_cast<uintptr_t>(copMgrPtr)), &angleRad, &dist);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }

        outDistance = dist;
        outBearingDeg = angleRad * 57.295776f;
        return true;
    }

    void SetupCopProximityTrigger() {
        g_triggerLatched = false;
        g_lastDist = -1.0f;
        g_lastSampleTimeMs = 0;
        g_lastTriggerTimeMs = 0;
        g_haveLastVelocity = false;
        g_lastVelX = g_lastVelY = g_lastVelZ = 0.0f;
        g_lastVelSampleTimeMs = 0;
    }

    void CleanupCopProximityTrigger() {
        // No detour installed -- nothing to detach.
    }

    void PollCopProximityTrigger(uintptr_t moduleBase) {
        float dist = 0.0f, bearingDeg = 0.0f;
        if (!TryGetNearestCopDistance(moduleBase, dist, bearingDeg)) {
            return;
        }

        ULONGLONG nowMs = GetTickCount64();

        if (dist <= 0.0f) {
            g_triggerLatched = false;
            g_lastDist = -1.0f;
            g_lastSampleTimeMs = 0;
            return;
        }

        g_lastDist = dist;
        g_lastSampleTimeMs = nowMs;

        float vx = 0.0f, vy = 0.0f, vz = 0.0f;
        bool haveVelocity = TryGetLocalPlayerVelocity(moduleBase, vx, vy, vz);
        float speed = haveVelocity ? std::sqrt(vx * vx + vy * vy + vz * vz) : 0.0f;

        float speedDrop = 0.0f;
        float angleChangeDeg = 0.0f;
        bool joltDetected = false;

        if (haveVelocity) {
            if (g_haveLastVelocity && g_lastVelSampleTimeMs != 0 && nowMs > g_lastVelSampleTimeMs) {
                float dtSec = static_cast<float>(nowMs - g_lastVelSampleTimeMs) / 1000.0f;
                if (dtSec > 0.0f && dtSec < 1.0f) {
                    float lastSpeed = std::sqrt(g_lastVelX * g_lastVelX + g_lastVelY * g_lastVelY + g_lastVelZ * g_lastVelZ);
                    speedDrop = lastSpeed - speed;

                    if (lastSpeed > kJoltMinSpeedForAngle && speed > kJoltMinSpeedForAngle) {
                        float dot = (g_lastVelX * vx + g_lastVelY * vy + g_lastVelZ * vz) / (lastSpeed * speed);
                        dot = std::fmax(-1.0f, std::fmin(1.0f, dot));
                        angleChangeDeg = std::acos(dot) * 57.295776f;
                    }

                    joltDetected = (speedDrop > kJoltMinSpeedDrop) || (angleChangeDeg > kJoltMinAngleChangeDeg);
                }
            }
            g_lastVelX = vx; g_lastVelY = vy; g_lastVelZ = vz;
            g_lastVelSampleTimeMs = nowMs;
            g_haveLastVelocity = true;
        }
        else {
            g_haveLastVelocity = false;
            g_lastVelSampleTimeMs = 0;
        }

        const float resetRadius = kTriggerRadius * 1.5f;
        bool cooldownElapsed = (g_lastTriggerTimeMs == 0) || (nowMs - g_lastTriggerTimeMs >= kCooldownMs);

        if (!g_triggerLatched && cooldownElapsed && dist < kTriggerRadius && joltDetected) {
            Logger::Instance().Info("[cop-proximity] Trigger: dist=%.2f bearing=%.1fdeg speedDrop=%.2f angleChange=%.1fdeg -- launching event id=%u",
                dist, bearingDeg, speedDrop, angleChangeDeg, kPursuitEventId);
            TryStartSinglePlayerEvent(moduleBase, kPursuitEventId, 1);
            g_triggerLatched = true;
            g_lastTriggerTimeMs = nowMs;
        }
        else if (g_triggerLatched && dist > resetRadius) {
            g_triggerLatched = false;
        }
    }

}
