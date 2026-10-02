#ifdef _DEBUG
// Debug build only: F7 launches the real pursuit event 385.
#include "Log.h"
#include "Game.h"

namespace Mod::Log {
    void LaunchTestEvent() {
        constexpr uint32_t kEventId = 385;
        uintptr_t self = Addr(Ida::ModeCtx);
        uint32_t vt = 0, getter = 0, mgr = 0, vt2 = 0, launch = 0;
        if (!ReadU32(self, vt) || !vt || !ReadU32(vt + 224, getter) || !getter) { LOG("[F7] context not ready"); return; }
        __try { mgr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(reinterpret_cast<void*(__stdcall*)(void*)>(getter)(reinterpret_cast<void*>(self)))); }
        __except (EXCEPTION_EXECUTE_HANDLER) { mgr = 0; }
        if (!mgr || !ReadU32(mgr, vt2) || !ReadU32(vt2 + 16, launch) || !launch) { LOG("[F7] event launcher not available"); return; }
        bool ok = true;
        __try { reinterpret_cast<void(__thiscall*)(void*, uint32_t, int)>(launch)(reinterpret_cast<void*>(mgr), kEventId, 1); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        LOG("[F7] launch event %u: %s", kEventId, ok ? "called" : "FAULTED");
        (void)ok;
    }
}
#endif
