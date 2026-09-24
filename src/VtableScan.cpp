#include "VtableScan.h"
#include "Logger.h"

#include <windows.h>
#include <cstdint>

namespace Probe {

    static bool SafeReadDword(const volatile uint32_t* address, uint32_t& outValue) {
        __try {
            outValue = *address;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // dword_CECC8C (module offset 0x8ECC8C) is the "current game mode
    // context" singleton. Its vtable slot 56 (+0xE0) getter returns a
    // "manager2" object whose vtable slot 4 (+0x10) is the launcher,
    // called as __thiscall(manager2, eventId, flag).
    bool TryStartSinglePlayerEvent(uintptr_t moduleBase, uint32_t eventId, int flag) {
        auto& log = Logger::Instance();

        constexpr uintptr_t kSingletonModuleOffset = 0xCECC8C - 0x400000;
        const uintptr_t thisAddr = moduleBase + kSingletonModuleOffset;

        uint32_t vtable1 = 0;
        if (!SafeReadDword(reinterpret_cast<const volatile uint32_t*>(thisAddr), vtable1) || vtable1 == 0) {
            log.Warn("[start-sp-event] Refusing: dword_CECC8C singleton not constructed yet.");
            return false;
        }

        uint32_t getterFn = 0;
        if (!SafeReadDword(reinterpret_cast<const volatile uint32_t*>(static_cast<uintptr_t>(vtable1) + 224), getterFn) || getterFn == 0) {
            log.Warn("[start-sp-event] Refusing: could not read slot 56 (+0xE0) getter.");
            return false;
        }

        uint32_t manager2 = 0;
        __try {
            using Getter_t = void* (__stdcall*)(void*);
            Getter_t getter = reinterpret_cast<Getter_t>(static_cast<uintptr_t>(getterFn));
            manager2 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(getter(reinterpret_cast<void*>(thisAddr))));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            log.Warn("[start-sp-event] Structured exception calling the getter.");
            return false;
        }

        if (manager2 == 0) {
            log.Warn("[start-sp-event] Getter returned null.");
            return false;
        }

        uint32_t vtable2 = 0;
        if (!SafeReadDword(reinterpret_cast<const volatile uint32_t*>(static_cast<uintptr_t>(manager2)), vtable2) || vtable2 == 0) {
            log.Warn("[start-sp-event] Refusing: could not read vtable of manager2.");
            return false;
        }

        uint32_t launcherFn = 0;
        if (!SafeReadDword(reinterpret_cast<const volatile uint32_t*>(static_cast<uintptr_t>(vtable2) + 16), launcherFn) || launcherFn == 0) {
            log.Warn("[start-sp-event] Refusing: could not read slot 4 (+0x10) launcher.");
            return false;
        }

        __try {
            using Launcher_t = void(__thiscall*)(void*, uint32_t, int);
            Launcher_t launcher = reinterpret_cast<Launcher_t>(static_cast<uintptr_t>(launcherFn));
            launcher(reinterpret_cast<void*>(static_cast<uintptr_t>(manager2)), eventId, flag);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            log.Warn("[start-sp-event] Structured exception calling the launcher.");
            return false;
        }
    }

}
