// Timer del livello di calore come nella beta 2010: nel PursuitIndicator (badge del calore + timer) il gioco 2015
// chiama ancora pursuitIndicator.setWaveLevel / setWaveTimer (0x48DF80), ma il valore del timer che riceve non
// conta piu' (resta a 1.0). Questo modulo calcola il tempo che manca al prossimo livello e lo scrive nei campi
// che 0x48DF80 passa all'HUD, subito dopo che il gioco li ha letti.
//
// Ritmo del calore come nella beta (feature "heatpacing", attiva di default): durante l'inseguimento il livello
// di calore lo decide l'ASI. Ogni livello dura il tempo della beta luglio 2010 (pursuitlevels heat_0N, campo
// 0xAC6E1EC7 = durata dell'ondata, letto dal codice della beta in 0x866390): 150 s al livello 1, 120 s dal 2 al 5,
// poi 45/90/60/45 s. Se il gioco alza il calore prima del tempo viene riportato al livello in corso; se lo abbassa
// (es. arresto) si riparte da li'. I tempi si cambiano con righe "level <n> <secondi>" in
// NFSWorldPursuitProbe_heattimer.txt; "maxlevel <n>" limita il livello massimo (default 10).
//
// L'indicatore del calore si riempie in base al tempo trascorso nel livello (calore = livello + frazione),
// feature "heatfill" (attiva di default; disattivandola il calore resta fermo sull'intero come prima).
//
// Con "heatpacing" disattivato (NFSWorldPursuitProbe_disable.txt) il calore resta al gioco e il timer viene solo
// stimato: crescita continua -> (prossimo intero - calore) / velocita'; a scatti -> intervallo misurato tra due
// scatti (prima del primo: "interval <secondi>", default 180).
#include "HeatTimer.h"
#include "Game.h"
#include "Features.h"
#include "Log.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace Mod::HeatTimer {
    constexpr uint32_t kPatchVa  = 0x48DFF4;   // dentro 0x48DF80, subito dopo la chiamata che riempie this+0x68..
    constexpr uint32_t kReturnVa = 0x48DFF9;
    static const uint8_t kOriginal[5] = { 0x8B, 0x46, 0x78, 0x33, 0xFF };   // mov eax,[esi+78h] / xor edi,edi
    constexpr int kMaxLevel = 10;

    // condivisi con il codice chiamato dall'HUD (stesso thread di gioco)
    static volatile LONG g_on = 0;
    static volatile ULONGLONG g_deadlineMs = 0;   // istante in cui scatta il prossimo livello (0 = timer fermo a zero)
    static volatile LONG g_level = 1;
    static uint32_t g_ret = 0;
    static bool g_patched = false;

    static float g_defaultInterval = 180.0f;
    // durata di ogni livello (s) dalla beta luglio 2010; indice = livello
    static float g_levelSecs[kMaxLevel + 1] = { 0, 150, 120, 120, 120, 120, 45, 90, 60, 45, 60 };
    static bool g_pacing = true;
    static int g_maxLevel = 10;   // livello massimo raggiungibile ("maxlevel <n>")
    static int g_paceLevel = 0;
    static ULONGLONG g_paceStepMs = 0;
    // ultimo calore raggiunto (livello + frazione): il prossimo inseguimento riparte da qui.
    // Salvato anche su file (NFSWorldPursuitProbe_heat.txt) per ritrovarlo dopo un riavvio del gioco.
    static float g_savedHeat = 0.0f;
    static ULONGLONG g_nextSaveMs = 0;
    static std::string SavePath() { return ModuleDir() + "NFSWorldPursuitProbe_heat.txt"; }
    static void WriteSaved() {
        FILE* f = nullptr;
        if (fopen_s(&f, SavePath().c_str(), "w") != 0 || !f) return;
        fprintf(f, "%.4f\n", g_savedHeat);
        fclose(f);
    }
    static void ReadSaved() {
        FILE* f = nullptr;
        if (fopen_s(&f, SavePath().c_str(), "r") != 0 || !f) return;
        float v = 0.0f;
        if (fscanf_s(f, "%f", &v) == 1 && v >= 1.0f && v <= static_cast<float>(kMaxLevel)) g_savedHeat = v;
        fclose(f);
    }

    // stato del calcolo
    static bool g_wasActive = false;
    static int g_lastLevel = 0;
    static ULONGLONG g_lastStepMs = 0;
    static float g_interval = 180.0f;
    static bool g_learned = false;
    constexpr int kSamples = 16;
    static ULONGLONG g_sampleMs[kSamples] = {};
    static float g_sampleHeat[kSamples] = {};
    static int g_sampleCount = 0, g_sampleHead = 0;
    static ULONGLONG g_nextLogMs = 0;

    // Chiamata a ogni aggiornamento dell'HUD: this+116 = secondi al prossimo livello, this+120 = livello del badge.
    static void __stdcall FillHud(uint8_t* self) {
        if (!g_on) return;
        float t = 0.0f;
        ULONGLONG dl = g_deadlineMs, now = GetTickCount64();
        if (dl > now) t = static_cast<float>(dl - now) / 1000.0f;
        *reinterpret_cast<float*>(self + 0x74) = t;
        *reinterpret_cast<int32_t*>(self + 0x78) = g_level;
    }

    static __declspec(naked) void Cave() {
        __asm {
            pushfd
            pushad
            push esi
            call FillHud
            popad
            popfd
            mov eax, [esi + 0x78]
            xor edi, edi
            jmp dword ptr [g_ret]
        }
    }

    static void LoadConfig() {
        FILE* f = nullptr;
        std::string path = ModuleDir() + "NFSWorldPursuitProbe_heattimer.txt";
        if (fopen_s(&f, path.c_str(), "r") != 0 || !f) return;
        char line[96];
        while (fgets(line, sizeof(line), f)) {
            float v = 0;
            int lv = 0;
            if (sscanf_s(line, "interval %f", &v) == 1 && v > 1.0f) g_defaultInterval = v;
            else if (sscanf_s(line, "maxlevel %d", &lv) == 1 && lv >= 1 && lv <= kMaxLevel) g_maxLevel = lv;
            else if (sscanf_s(line, "level %d %f", &lv, &v) == 2 && lv >= 1 && lv <= kMaxLevel && v > 1.0f) g_levelSecs[lv] = v;
        }
        fclose(f);
        LOG("[heattimer] config: intervallo iniziale %.1f s, livelli %.0f/%.0f/%.0f/%.0f/%.0f/%.0f/%.0f/%.0f/%.0f/%.0f s",
            g_defaultInterval, g_levelSecs[1], g_levelSecs[2], g_levelSecs[3], g_levelSecs[4], g_levelSecs[5],
            g_levelSecs[6], g_levelSecs[7], g_levelSecs[8], g_levelSecs[9], g_levelSecs[10]);
    }

    void Install() {
        if (!Features::On("heattimer")) { LOG("[heattimer] disattivato"); return; }
        g_pacing = Features::On("heatpacing");
        LoadConfig();
        ReadSaved();
        LOG("[heattimer] calore salvato all'avvio: %.3f", g_savedHeat);
        LOG("[heattimer] ritmo del calore della beta: %s", g_pacing ? "ON" : "OFF");
        uint8_t* p = reinterpret_cast<uint8_t*>(Addr(kPatchVa));
        if (memcmp(p, kOriginal, sizeof(kOriginal)) != 0) {
            LOG("[heattimer] byte inattesi a 0x%08X, patch non applicata", kPatchVa);
            return;
        }
        g_ret = Addr32(kReturnVa);
        DWORD old = 0;
        if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return;
        p[0] = 0xE9;
        *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&Cave) - (reinterpret_cast<uintptr_t>(p) + 5));
        VirtualProtect(p, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
        g_patched = true;
        LOG("[heattimer] patch HUD installata (0x%08X)", kPatchVa);
    }

    void ResetSaved(float heat) {
        g_savedHeat = heat;
        WriteSaved();
        LOG("[heattimer] calore salvato azzerato a %.2f", heat);
    }

    void Remove() {
        g_on = 0;
        if (!g_patched) return;
        uint8_t* p = reinterpret_cast<uint8_t*>(Addr(kPatchVa));
        DWORD old = 0;
        if (VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(p, kOriginal, sizeof(kOriginal));
            VirtualProtect(p, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p, 5);
        }
        g_patched = false;
    }

    static void Reset(ULONGLONG now, int level) {
        g_lastLevel = level;
        g_lastStepMs = now;
        g_interval = g_defaultInterval;
        g_learned = false;
        g_sampleCount = 0; g_sampleHead = 0;
    }

    // Ritmo della beta: restituisce il calore da imporre o -1.
    static ULONGLONG g_lastTickMs = 0;
    static float TickPacing(ULONGLONG now, float heat, bool paused) {
        if (paused && g_paceLevel && g_lastTickMs) g_paceStepMs += now - g_lastTickMs;   // durante il raffreddamento il tempo non scorre
        g_lastTickMs = now;
        int gameLevel = static_cast<int>(std::floor(heat + 0.0001f));
        if (gameLevel < 1) gameLevel = 1;
        if (g_paceLevel == 0) {
            // riparte dal calore salvato alla fine dell'inseguimento precedente (se il gioco non e' gia' piu' in alto)
            float start = heat;
            if (Features::On("heatkeep") && g_savedHeat > start) start = g_savedHeat;
            int lv = static_cast<int>(std::floor(start + 0.0001f));
            if (lv < 1) lv = 1;
            if (lv > g_maxLevel) lv = g_maxLevel;
            float frac = start - static_cast<float>(lv);
            if (frac < 0.0f || lv >= g_maxLevel) frac = 0.0f;
            if (frac > 0.995f) frac = 0.995f;
            g_paceLevel = lv;
            g_paceStepMs = now - static_cast<ULONGLONG>(frac * g_levelSecs[lv] * 1000.0f);
            LOG("[heattimer] inizio: calore del gioco %.3f, salvato %.3f -> livello %d al %.0f%% (%.0f s per livello)",
                heat, g_savedHeat, g_paceLevel, frac * 100.0f, g_levelSecs[g_paceLevel]);
        }
        else if (gameLevel < g_paceLevel) {        // il gioco ha abbassato il calore (es. arresto): si riparte da li'
            LOG("[heattimer] calore sceso dal gioco: livello %d -> %d", g_paceLevel, gameLevel);
            g_paceLevel = gameLevel;
            g_paceStepMs = now;
        }
        float dur = g_levelSecs[g_paceLevel];
        float elapsed = static_cast<float>(now - g_paceStepMs) / 1000.0f;
        if (g_paceLevel < g_maxLevel && elapsed >= dur) {
            ++g_paceLevel;
            g_paceStepMs = now;
            elapsed = 0.0f;
            dur = g_levelSecs[g_paceLevel];
            LOG("[heattimer] livello %d (prossimo tra %.0f s)", g_paceLevel, dur);
        }
        float remaining = g_paceLevel >= g_maxLevel ? 0.0f : dur - elapsed;
        if (remaining < 0.0f) remaining = 0.0f;
        g_deadlineMs = remaining > 0.0f ? now + static_cast<ULONGLONG>(remaining * 1000.0f) : 0;
        g_level = g_paceLevel;
        g_on = 1;
        if (::Mod::Log::Enabled() && now >= g_nextLogMs) {
            g_nextLogMs = now + 5000;
            LOG("[heattimer] calore del gioco %.3f, livello %d, mancano %.1f s", heat, g_paceLevel, remaining);
        }
        // il gioco non deve salire da solo: il calore segue il timer del livello in corso, cosi' l'indicatore
        // si riempie gradualmente (livello + frazione di tempo trascorsa) e scatta al livello successivo a zero
        float target = static_cast<float>(g_paceLevel);
        if (g_paceLevel < g_maxLevel && dur > 0.0f && Features::On("heatfill")) {
            float frac = elapsed / dur;
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 0.995f) frac = 0.995f;
            target += frac;
        }
        g_savedHeat = target;
        if (now >= g_nextSaveMs) { g_nextSaveMs = now + 2000; WriteSaved(); }
        return std::fabs(heat - target) > 0.001f ? target : -1.0f;
    }

    float Tick(ULONGLONG now, bool active, float heat, bool paused) {
        if (!g_patched) return -1.0f;
        if (!active || heat < 0.0f) {
            if (g_wasActive) { LOG("[heattimer] fine inseguimento, calore salvato %.3f", g_savedHeat); WriteSaved(); }
            g_wasActive = false;
            g_paceLevel = 0;
            g_lastTickMs = 0;
            g_on = 0;
            return -1.0f;
        }
        if (g_pacing) { g_wasActive = true; return TickPacing(now, heat, paused); }
        int level = static_cast<int>(std::floor(heat + 0.0001f));
        if (level < 1) level = 1;
        if (!g_wasActive) {
            g_wasActive = true;
            Reset(now, level);
            LOG("[heattimer] inizio: calore %.3f, livello %d, intervallo %.1f s", heat, level, g_interval);
        }
        if (level < g_lastLevel) {               // calore azzerato (es. arresto)
            Reset(now, level);
        }
        else if (level > g_lastLevel) {          // scatto di livello: misura l'intervallo
            float elapsed = static_cast<float>(now - g_lastStepMs) / 1000.0f / static_cast<float>(level - g_lastLevel);
            if (elapsed > 2.0f) { g_interval = elapsed; g_learned = true; }
            LOG("[heattimer] livello %d -> %d dopo %.1f s (intervallo ora %.1f s)", g_lastLevel, level, elapsed, g_interval);
            g_lastLevel = level;
            g_lastStepMs = now;
            g_sampleCount = 0; g_sampleHead = 0;
        }

        // campioni per la crescita continua
        g_sampleMs[g_sampleHead] = now; g_sampleHeat[g_sampleHead] = heat;
        g_sampleHead = (g_sampleHead + 1) % kSamples;
        if (g_sampleCount < kSamples) ++g_sampleCount;
        float rate = 0.0f;
        if (g_sampleCount >= 6) {
            int oldest = (g_sampleHead - g_sampleCount + kSamples) % kSamples;
            float dt = static_cast<float>(now - g_sampleMs[oldest]) / 1000.0f;
            float dh = heat - g_sampleHeat[oldest];
            if (dt > 0.5f && dh > 0.0005f) rate = dh / dt;
        }

        float remaining;
        if (level >= kMaxLevel) remaining = 0.0f;
        else if (rate > 0.0f) remaining = (static_cast<float>(level + 1) - heat) / rate;
        else remaining = g_interval - static_cast<float>(now - g_lastStepMs) / 1000.0f;
        if (remaining < 0.0f) remaining = 0.0f;
        if (remaining > 5999.0f) remaining = 5999.0f;

        g_deadlineMs = remaining > 0.0f ? now + static_cast<ULONGLONG>(remaining * 1000.0f) : 0;
        g_level = level;
        g_on = 1;

        if (::Mod::Log::Enabled() && now >= g_nextLogMs) {
            g_nextLogMs = now + 5000;
            LOG("[heattimer] calore %.3f livello %d, %s %.4f/s, mancano %.1f s", heat, level,
                rate > 0.0f ? "crescita continua" : (g_learned ? "intervallo misurato" : "intervallo iniziale"), rate, remaining);
        }
        return -1.0f;
    }
}
