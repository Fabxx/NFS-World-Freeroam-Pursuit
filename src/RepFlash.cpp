// REP dell'inseguimento nei flasher, come nella beta: "Property Damage <stella> +50".
// L'ASI calcola la REP guadagnata finora con la stessa formula della schermata dei risultati; quando sale,
// invia l'aumento a flashers.gfx (flasherController.AddRepBonus), che lo mostra accanto al flasher.
//
// v0.4b: l'invio non e' piu' immediato.
//  - la REP calcolata viene accumulata e inviata solo quando il calcolo si e' stabilizzato (kSettleMs senza
//    nuovi aumenti), cosi' un evento che fa salire la REP in piu' passi arriva come un unico "+N";
//  - l'aumento viene legato al flasher dell'evento (quello aggiunto poco prima dell'aumento, o il primo aggiunto
//    dopo) e inviato solo quando quel flasher e' il messaggio corrente, cioe' dopo che i flasher in coda prima
//    di lui sono stati mostrati e chiusi. Lo stato della coda arriva dai callback OnFlasherStarted /
//    OnFlasherFinished del gadget (sub_482E70): la REP non viene mai inviata mentre e' a schermo un messaggio
//    precedente, e non finisce piu' nel "pending" di flashers.gfx dove poteva scadere o finire sul flasher sbagliato.
#include "RepFlash.h"
#include "Game.h"
#include "Hooks.h"
#include "Pursuit.h"
#include "Results.h"
#include "Features.h"
#include "Log.h"
#include <cstring>

namespace Mod::RepFlash {
    constexpr uint32_t kAddFlasher = 0x4818A0;   // FlasherGadget::AddFlasher(this, flasher), thiscall, ret 4
    constexpr uint32_t kOnCallback = 0x482E70;   // FlasherGadget ExternalInterface(this, movie, method, args, nargs), ret 0x10
    constexpr uint32_t kMovieInvoke = 0x9A7930;  // GFxMovie::Invoke(this = movie, method, args, nargs)
    constexpr uint32_t kGadgetMovie = 0x2C;      // movie del gadget dei flasher
    constexpr uint32_t kValueNumber = 3;

    constexpr ULONGLONG kSettleMs = 250;         // la REP deve restare ferma per questo tempo prima dell'invio
    constexpr ULONGLONG kBindWindowMs = 1500;    // un flasher aggiunto fino a questo tempo prima dell'aumento e' "il suo"
    constexpr ULONGLONG kShowDelayMs = 60;       // attende che Show() del flasher sia completato
    constexpr ULONGLONG kShowWindowMs = 1300;    // flashers.gfx accetta la REP sul flasher corrente per 1500 ms
    constexpr ULONGLONG kNearMs = 400;           // flasher aggiunto cosi' vicino prima dell'aumento: e' sicuramente il suo
    constexpr ULONGLONG kWaitAfterMs = 500;      // altrimenti aspetta questo tempo un eventuale flasher aggiunto dopo
    constexpr uint32_t kRing = 64;

    struct GFxValue { void* object; uint32_t type; double number; };

    using AddFlasher_t = void(__thiscall*)(void*, void*);
    using OnCallback_t = void(__thiscall*)(void*, void*, const char*, GFxValue*, unsigned);
    static AddFlasher_t g_origAdd = nullptr;
    static OnCallback_t g_origCb = nullptr;
    static volatile uint32_t g_gadget = 0, g_gadgetVt = 0;

    // Coda dei flasher: ogni AddFlasher riceve un numero di sequenza; flashers.gfx li mostra in ordine (FIFO),
    // quindi l'n-esimo OnFlasherStarted corrisponde all'n-esimo flasher aggiunto.
    static CRITICAL_SECTION g_cs;
    static bool g_csInit = false;
    static uint32_t g_addSeq = 0;                  // ultimo numero assegnato
    static ULONGLONG g_addTime[kRing] = {};        // istante di aggiunta per seq % kRing
    static uint32_t g_startSeq = 0;                // ultimo flasher diventato "corrente"
    static uint32_t g_curSeq = 0;                  // flasher a schermo (0 = nessuno)
    static ULONGLONG g_curStartMs = 0;
    static int g_curId = -1;

    // REP in attesa
    static int g_lastRep = -1;
    static int g_pending = 0;
    static ULONGLONG g_firstGainMs = 0, g_lastGainMs = 0;
    static uint32_t g_seqAtGain = 0;               // g_addSeq al primo aumento non ancora legato
    static uint32_t g_target = 0;                  // seq del flasher a cui va la REP (0 = non legato)
    static ULONGLONG g_pursuitStart = 0;

    struct Lock {
        Lock() { if (g_csInit) EnterCriticalSection(&g_cs); }
        ~Lock() { if (g_csInit) LeaveCriticalSection(&g_cs); }
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
    };

    static void ResetPending() { g_pending = 0; g_target = 0; g_seqAtGain = 0; g_firstGainMs = g_lastGainMs = 0; }

    // la REP non legata ricomincia a cercare il proprio flasher da "adesso"
    static void Unbind(ULONGLONG now) { g_target = 0; g_seqAtGain = g_addSeq; g_firstGainMs = now; }

    static void OnAdded() {
        Lock l;
        ++g_addSeq;
        g_addTime[g_addSeq % kRing] = GetTickCount64();
    }

    static void __fastcall AddFlasherHook(void* self, void*, void* flasher) {
        uint32_t vt = 0;
        if (self && ReadU32(reinterpret_cast<uintptr_t>(self), vt)) {
            g_gadgetVt = vt;
            g_gadget = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(self));
        }
        OnAdded();
        g_origAdd(self, flasher);
    }

    // 1 = OnFlasherStarted, 2 = OnFlasherFinished, 0 = altro
    static int ClassifyCallback(const char* method, const GFxValue* args, unsigned nargs, int& id) {
        __try {
            if (!method || !args || nargs < 1 || args[0].type != kValueNumber) return 0;
            id = static_cast<int>(args[0].number);
            if (strcmp(method, "OnFlasherStarted") == 0) return 1;
            if (strcmp(method, "OnFlasherFinished") == 0) return 2;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    static void __fastcall OnCallbackHook(void* self, void*, void* movie, const char* method, GFxValue* args, unsigned nargs) {
        int id = -1;
        const int kind = ClassifyCallback(method, args, nargs, id);
        if (kind == 1) {
            Lock l;
            // se dei flasher sono stati tolti dalla coda senza essere mostrati la sequenza non puo' superare l'ultimo aggiunto
            if (g_startSeq < g_addSeq) ++g_startSeq;
            g_curSeq = g_startSeq; g_curStartMs = GetTickCount64(); g_curId = id;
        }
        else if (kind == 2) {
            Lock l;
            if (id == g_curId) { g_curSeq = 0; g_curId = -1; }
        }
        g_origCb(self, movie, method, args, nargs);
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

    // Il flasher dell'evento e' quello aggiunto piu' vicino nel tempo al primo aumento della REP:
    //  - "prima": l'ultimo aggiunto prima dell'aumento (se non e' gia' stato chiuso), fino a kBindWindowMs prima;
    //  - "dopo": il primo aggiunto dopo l'aumento (il flasher puo' arrivare dopo l'aggiornamento delle statistiche).
    // Se c'e' solo un candidato "prima" ma lontano nel tempo, aspetta un po' l'eventuale flasher "dopo".
    static void TryBind(ULONGLONG now) {
        if (g_target) return;
        const uint32_t s = g_seqAtGain;
        uint32_t before = 0, after = 0;
        ULONGLONG dtBefore = 0, dtAfter = 0;
        if (s && s + kRing > g_addSeq && (s > g_startSeq || s == g_curSeq)) {
            const ULONGLONG t = g_addTime[s % kRing];
            if (g_firstGainMs >= t && g_firstGainMs - t <= kBindWindowMs) { before = s; dtBefore = g_firstGainMs - t; }
        }
        if (g_addSeq > s) {
            after = s + 1;
            const ULONGLONG t = g_addTime[after % kRing];
            dtAfter = t >= g_firstGainMs ? t - g_firstGainMs : 0;
        }
        if (after && dtAfter <= kBindWindowMs) { g_target = (before && dtBefore < dtAfter) ? before : after; return; }
        if (before) {
            if (dtBefore <= kNearMs || now - g_firstGainMs >= kWaitAfterMs) g_target = before;
            return;
        }
        if (after) g_target = after;                 // nessun flasher vicino: il primo che arriva
    }

    static void Process(int rep, ULONGLONG now) {
        if (g_lastRep < 0 || rep < g_lastRep) { g_lastRep = rep; ResetPending(); return; }
        if (rep > g_lastRep) {
            const int gain = rep - g_lastRep;
            g_lastRep = rep;
            if (g_pending == 0) { g_seqAtGain = g_addSeq; g_target = 0; g_firstGainMs = now; }
            g_pending += gain;
            g_lastGainMs = now;
            LOG("[repflash] +%d REP calcolata (totale inseguimento %d), in attesa %d", gain, rep, g_pending);
        }
        if (g_pending <= 0) return;

        // 1) aspetta che il calcolo della REP sia finito
        if (now - g_lastGainMs < kSettleMs) return;

        // 2) trova il flasher dell'evento (se non e' ancora arrivato, aspetta)
        TryBind(now);
        if (!g_target) return;

        // il flasher legato e' gia' stato mostrato e chiuso: la REP va al prossimo messaggio
        if (g_target <= g_startSeq && g_target != g_curSeq) {
            LOG("[repflash] flasher #%u gia' chiuso, REP %d al prossimo messaggio", g_target, g_pending);
            Unbind(now);
            return;
        }

        // 3) aspetta che i messaggi precedenti siano spariti e che il suo sia a schermo
        if (g_curSeq != g_target) return;
        const ULONGLONG shown = now - g_curStartMs;
        if (shown < kShowDelayMs) return;
        if (shown > kShowWindowMs) {
            LOG("[repflash] flasher #%u a schermo da troppo, REP %d al prossimo messaggio", g_target, g_pending);
            Unbind(now);
            return;
        }
        const int amount = g_pending;
        const bool sent = Send(amount);
        LOG("[repflash] +%d REP inviata al flasher #%u%s", amount, g_target, sent ? "" : " (gadget non pronto, ritento)");
        if (sent) ResetPending();
    }

    void Poll() {
        if (!Pursuit::IsActive() || !Features::On("repflasher")) { Lock l; g_lastRep = -1; ResetPending(); return; }
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG start = Pursuit::GetStats().startMs;
        const int rep = Results::PursuitRepSoFar();
        Lock l;
        if (start != g_pursuitStart) { g_pursuitStart = start; g_lastRep = -1; ResetPending(); }
        Process(rep, now);
    }

    void Install() {
        if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; }
        g_origAdd = reinterpret_cast<AddFlasher_t>(Addr(kAddFlasher));
        Hooks::Attach(reinterpret_cast<void**>(&g_origAdd), reinterpret_cast<void*>(AddFlasherHook), "flasher gadget addFlasher (REP)");
        g_origCb = reinterpret_cast<OnCallback_t>(Addr(kOnCallback));
        Hooks::Attach(reinterpret_cast<void**>(&g_origCb), reinterpret_cast<void*>(OnCallbackHook), "flasher gadget callbacks (REP)");
    }

    void Remove() {
        Hooks::Detach(reinterpret_cast<void**>(&g_origCb), reinterpret_cast<void*>(OnCallbackHook));
        Hooks::Detach(reinterpret_cast<void**>(&g_origAdd), reinterpret_cast<void*>(AddFlasherHook));
    }
}
