// REP dell'inseguimento nei flasher, come nella beta: "Property Damage <stella> +50".
// L'ASI calcola la REP guadagnata finora con la stessa formula della schermata dei risultati; quando sale,
// invia l'aumento a flashers.gfx (flasherController.AddRepBonus), che lo mostra accanto al flasher.
//
// v0.4b: funziona anche negli inseguimenti degli eventi (team escape): le statistiche arrivano dallo stesso
// HUD (UpdateCopInfo) tramite Stats::OnHud -> PollEvent, con la stessa formula della REP "Pursuit".
// L'invio non e' piu' immediato: ogni calcolo della REP concluso (kSettleMs senza nuovi aumenti) va in coda e
// le voci vengono inviate una per messaggio, nell'ordine, al prossimo flasher che compare a schermo (stato
// della coda dai callback OnFlasherStarted / OnFlasherFinished del gadget, sub_482E70). Un messaggio riceve
// al massimo una REP: azioni ravvicinate restano in coda e aspettano i messaggi successivi.
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
    constexpr ULONGLONG kShowDelayMs = 60;       // attende che Show() del flasher sia completato
    constexpr ULONGLONG kShowWindowMs = 1300;    // flashers.gfx accetta la REP sul flasher corrente per 1500 ms
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
    static uint32_t g_lastSentSeq = 0;             // ultimo flasher che ha gia' ricevuto la sua REP: non ne riceve altra

    struct Lock {
        Lock() { if (g_csInit) EnterCriticalSection(&g_cs); }
        ~Lock() { if (g_csInit) LeaveCriticalSection(&g_cs); }
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
    };

    static void ResetPending();

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

    // Coda delle REP calcolate: ogni calcolo concluso (kSettleMs senza nuovi aumenti) diventa una voce;
    // le voci vengono inviate una per messaggio, in ordine, ciascuna al prossimo flasher che compare a schermo.
    // Un flasher che ha gia' ricevuto (o saltato) la sua REP non ne riceve altre, quindi azioni ravvicinate
    // non si sommano piu' sullo stesso messaggio.
    constexpr int kQueueMax = 16;
    struct Entry { int amount; ULONGLONG gainMs; };
    static Entry g_queue[kQueueMax];
    static int g_qHead = 0, g_qCount = 0;

    static void QueuePush(int amount, ULONGLONG gainMs) {
        if (g_qCount == kQueueMax) {                 // coda piena: si somma all'ultima voce
            g_queue[(g_qHead + g_qCount - 1) % kQueueMax].amount += amount;
            return;
        }
        g_queue[(g_qHead + g_qCount) % kQueueMax] = Entry{ amount, gainMs };
        ++g_qCount;
    }

    static void ResetPending() { g_pending = 0; g_firstGainMs = g_lastGainMs = 0; g_qHead = 0; g_qCount = 0; }

    static void Process(int rep, ULONGLONG now) {
        if (g_lastRep < 0 || rep < g_lastRep) { g_lastRep = rep; ResetPending(); return; }
        if (rep > g_lastRep) {
            const int gain = rep - g_lastRep;
            g_lastRep = rep;
            if (g_pending == 0) g_firstGainMs = now;
            g_pending += gain;
            g_lastGainMs = now;
            LOG("[repflash] +%d REP calcolata (totale inseguimento %d)", gain, rep);
        }

        // 1) calcolo concluso -> nuova voce in coda
        if (g_pending > 0 && now - g_lastGainMs >= kSettleMs) {
            QueuePush(g_pending, g_firstGainMs);
            LOG("[repflash] REP %d in coda (%d in attesa)", g_pending, g_qCount);
            g_pending = 0; g_firstGainMs = g_lastGainMs = 0;
        }
        if (g_qCount == 0) return;

        // 2) la prima voce va al prossimo messaggio che compare (mai a uno che ha gia' avuto la sua REP)
        if (!g_curSeq || g_curSeq <= g_lastSentSeq) return;
        const ULONGLONG shown = now - g_curStartMs;
        if (shown < kShowDelayMs) return;
        if (shown > kShowWindowMs) {                 // a schermo da troppo: flashers.gfx non la mostrerebbe piu'
            LOG("[repflash] flasher #%u a schermo da troppo, la REP aspetta il prossimo messaggio", g_curSeq);
            g_lastSentSeq = g_curSeq;
            return;
        }
        const Entry e = g_queue[g_qHead];
        const bool sent = Send(e.amount);
        LOG("[repflash] +%d REP inviata al flasher #%u%s (%d ancora in coda)", e.amount, g_curSeq,
            sent ? "" : " (gadget non pronto, ritento)", sent ? g_qCount - 1 : g_qCount);
        if (sent) {
            g_lastSentSeq = g_curSeq;
            g_qHead = (g_qHead + 1) % kQueueMax;
            --g_qCount;
        }
    }

    // Chiave della sessione: inizio dell'inseguimento freeroam, oppure sessione dell'evento (bit alto) per i team escape.
    static ULONGLONG g_session = 0;
    static void Run(ULONGLONG session, int rep) {
        const ULONGLONG now = GetTickCount64();
        Lock l;
        if (session != g_session) { g_session = session; g_lastRep = -1; ResetPending(); }
        Process(rep, now);
    }

    void Poll() {
        if (!Pursuit::IsActive() || !Features::On("repflasher")) { Lock l; g_lastRep = -1; ResetPending(); return; }
        const ULONGLONG start = Pursuit::GetStats().startMs;
        Run(start, Results::PursuitRepSoFar());
    }

    void PollEvent(const ::Mod::Stats::Snapshot& hs, uint32_t session) {
        if (!Features::On("repflasher") || !Features::On("eventrepflasher")) return;
        Run(0x8000000000000000ULL | session, Results::EventRepSoFar(hs));
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
