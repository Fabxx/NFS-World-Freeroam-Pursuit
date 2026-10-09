// REP dell'inseguimento nei flasher, come nella beta: "Property Damage <stella> +50".
// L'ASI calcola la REP guadagnata finora con la stessa formula della schermata dei risultati; quando sale,
// invia l'aumento a flashers.gfx (flasherController.AddRepBonus), che lo mostra accanto al flasher in corso.
#include "RepFlash.h"
#include "Game.h"
#include "Hooks.h"
#include "Pursuit.h"
#include "Results.h"
#include "Features.h"
#include "Log.h"

namespace Mod::RepFlash {
    constexpr uint32_t kAddFlasher = 0x4818A0;   // FlasherGadget::AddFlasher(this, flasher), thiscall, ret 4
    constexpr uint32_t kMovieInvoke = 0x9A7930;  // GFxMovie::Invoke(this = movie, method, args, nargs)
    constexpr uint32_t kGadgetMovie = 0x2C;      // movie del gadget dei flasher
    constexpr uint32_t kValueNumber = 3;

    struct GFxValue { void* object; uint32_t type; double number; };

    using AddFlasher_t = void(__thiscall*)(void*, void*);
    static AddFlasher_t g_origAdd = nullptr;
    static volatile uint32_t g_gadget = 0, g_gadgetVt = 0;
    static int g_lastRep = -1;
    static ULONGLONG g_pursuitStart = 0;

    static void __fastcall AddFlasherHook(void* self, void*, void* flasher) {
        uint32_t vt = 0;
        if (self && ReadU32(reinterpret_cast<uintptr_t>(self), vt)) {
            g_gadgetVt = vt;
            g_gadget = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
        }
        g_origAdd(self, flasher);
    }

    static bool Send(int amount) {
        uint32_t gadget = g_gadget, vt = 0, movie = 0;
        if (!gadget || !ReadU32(gadget, vt) || vt != g_gadgetVt || !ReadU32(gadget + kGadgetMovie, movie) || !movie) return false;
        GFxValue arg = { nullptr, kValueNumber, static_cast<double>(amount) };
        __try {
            reinterpret_cast<bool(__thiscall*)(uint32_t, const char*, GFxValue*, unsigned)>(Addr(kMovieInvoke))(
                movie, "flasherController.AddRepBonus", &arg, 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return true;
    }

    void Poll() {
        if (!Pursuit::IsActive() || !Features::On("repflasher")) { g_lastRep = -1; return; }
        const ULONGLONG start = Pursuit::GetStats().startMs;
        if (start != g_pursuitStart) { g_pursuitStart = start; g_lastRep = -1; }
        const int rep = Results::PursuitRepSoFar();
        if (g_lastRep < 0 || rep < g_lastRep) { g_lastRep = rep; return; }
        if (rep == g_lastRep) return;
        const int gain = rep - g_lastRep;
        g_lastRep = rep;
        const bool sent = Send(gain);
        LOG("[repflash] +%d REP (pursuit total %d)%s", gain, rep, sent ? "" : ", flasher gadget not ready");
        (void)sent;
    }

    void Install() {
        g_origAdd = reinterpret_cast<AddFlasher_t>(Addr(kAddFlasher));
        Hooks::Attach(reinterpret_cast<void**>(&g_origAdd), reinterpret_cast<void*>(AddFlasherHook), "flasher gadget addFlasher (REP)");
    }

    void Remove() { Hooks::Detach(reinterpret_cast<void**>(&g_origAdd), reinterpret_cast<void*>(AddFlasherHook)); }
}
