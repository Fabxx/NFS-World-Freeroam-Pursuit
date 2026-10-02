// Pursuit music: event Attrib collection, forced song and music source.
#include "Music.h"
#include "Game.h"
#include "Hooks.h"
#include "Log.h"
#include "Features.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace Mod::Music {
    constexpr char kMaskBit = 20;
    constexpr ULONGLONG kPulseMs = 400;
    constexpr uint32_t kEventSettingsClass = 40595996;
    constexpr int kSndWords = 8;

    static ULONGLONG g_pulseOffDueMs = 0;

    static uint32_t g_origPlayerUpdate = 0;
    static volatile uint32_t g_player = 0;

    static __declspec(naked) void PlayerUpdateStub() {
        __asm {
            mov  g_player, ecx
            jmp  dword ptr [g_origPlayerUpdate]
        }
    }

    static uint32_t g_sndApplied = 0;
    static uint32_t g_sndSaved[kSndWords] = {};
    static bool g_songApplied = false;
    static uint32_t g_songSaved = 0;
    static uint32_t g_srcApplied = 0;
    static uint8_t g_srcSaved64 = 0;

    static void CallMask(bool on) {
        __try { reinterpret_cast<void(__cdecl*)(char, char)>(Addr(Ida::MusicMask))(on ? 1 : 0, kMaskBit); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void BeginPulse(ULONGLONG now) {
        CallMask(true);
        g_pulseOffDueMs = now + kPulseMs;
    }

    constexpr uint32_t kMusicFlags = 0xD77EB0;
    static ULONGLONG g_diagDueMs[2] = {};

    // Music logic state for the log: state name, flags, chosen song.
    static void LogState(const char* when) {
        if (!::Mod::Log::Enabled()) return;
        uint32_t player = g_player, state = 0, name = 0, flags = 0, song = 0, mode = 0, b904 = 0;
        char text[48] = "?";
        ReadU32(Addr(kMusicFlags), flags);
        if (player) {
            ReadU32(player + 876, song);
            ReadU32(player + 736, mode);
            ReadU32(player + 904, b904);
            if (ReadU32(player + 48, state) && state && ReadU32(state + 28, name) && name) {
                __try { strncpy_s(text, reinterpret_cast<const char*>(static_cast<uintptr_t>(name)), _TRUNCATE); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
        LOG("[music] %s: %s, flags %08X, song %d, mode %u, started %u/%u", when, text, flags, static_cast<int>(song), mode,
            b904 & 0xFF, (b904 >> 8) & 0xFF);
        (void)when;
    }

    void Tick(ULONGLONG now) {
        for (int i = 0; i < 2; ++i)
            if (g_diagDueMs[i] && now >= g_diagDueMs[i]) { g_diagDueMs[i] = 0; LogState(i ? "after 6s" : "after 2s"); }
        if (!g_pulseOffDueMs || now < g_pulseOffDueMs) return;
        g_pulseOffDueMs = 0;
        CallMask(false);
    }

    static bool ReadWords(uint32_t base, uint32_t* out, int n) {
        for (int i = 0; i < n; ++i) if (!ReadU32(base + 4 * i, out[i])) return false;
        return true;
    }

    static void ApplyAttrib(uint32_t eventKey) {
        uint32_t mgr = 0, snd = 0, coll = 0, layout = 0, cur[kSndWords];
        if (g_sndApplied || !ReadU32(Addr(Ida::SoundMgr), mgr) || !mgr || !ReadU32(mgr + 200, snd) || !snd) return;
        __try { coll = CallAny2(Addr32(Ida::AttribCollection), 0, kEventSettingsClass, eventKey); }
        __except (EXCEPTION_EXECUTE_HANDLER) { coll = 0; }
        if (!coll || !ReadU32(coll + 28, layout) || !ReadWords(snd, cur, kSndWords)) {
            LOG("[music] event Attrib collection for key %08X not found -- pursuit music stays freeroam", eventKey);
            return;
        }
        const uint32_t mb = static_cast<uint32_t>(ExeBase());
        auto inImage = [mb](uint32_t v) { return v >= mb && v < mb + 0x1000000; };
        uint32_t curLayout = 0, curW0 = 0, newW0 = 0;
        bool ok = cur[0] && !inImage(cur[0]) && ReadU32(cur[0] + 28, curLayout) && curLayout == cur[1] &&
                  ReadU32(cur[0], curW0) && !inImage(curW0) && ReadU32(coll, newW0) && !inImage(newW0) &&
                  !inImage(cur[1]) && cur[0] != snd;
        const bool pair67 = !inImage(cur[6]) && !inImage(cur[7]);
        if (!ok || !pair67) {
            LOG("[music] snd 0x%08X is not an Attrib instance now (%08X %08X .. %08X %08X, *w0 %08X) -- pursuit music not applied",
                snd, cur[0], cur[1], cur[6], cur[7], curW0);
            return;
        }
        uint32_t want[kSndWords];
        memcpy(want, cur, sizeof(cur));
        want[0] = coll; want[1] = layout; want[6] = cur[0]; want[7] = cur[1];
        memcpy(g_sndSaved, cur, sizeof(cur));
        __try {
            for (int i = 0; i < kSndWords; ++i) reinterpret_cast<volatile uint32_t*>(snd)[i] = want[i];
            g_sndApplied = snd;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        LOG("[music] snd 0x%08X -> event collection 0x%08X (layout 0x%08X) | was %08X %08X .. %08X %08X *w0 %08X, new *w0 %08X",
            snd, coll, layout, g_sndSaved[0], g_sndSaved[1], g_sndSaved[6], g_sndSaved[7], curW0, newW0);
    }

    // Pursuit song slots that really have music: NFSWorldPursuitProbe_songs.txt (created with "0 1").
    constexpr int kMaxSongs = 6;
    static int g_songs[kMaxSongs] = {};
    static int g_songCount = 0;
    static int g_lastSong = -1;

    static void LoadSongs() {
        const std::string path = ModuleDir() + "NFSWorldPursuitProbe_songs.txt";
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "r") == 0 && f) {
            char line[128];
            while (fgets(line, sizeof(line), f)) {
                if (line[0] == '#') continue;
                const char* p = line;
                int v = 0, n = 0;
                while (g_songCount < kMaxSongs && sscanf_s(p, "%d%n", &v, &n) == 1) {
                    if (v >= 0 && v < kMaxSongs) g_songs[g_songCount++] = v;
                    p += n;
                }
            }
            fclose(f);
        }
        else if (fopen_s(&f, path.c_str(), "w") == 0 && f) {
            fprintf(f, "# Pursuit song slots (0-5) the pursuit music may use, separated by spaces.\n"
                       "# Remove a slot if its pursuit stays silent (the log says \"pursuit song N\").\n0 1\n");
            fclose(f);
        }
        if (!g_songCount) { g_songs[0] = 0; g_songs[1] = 1; g_songCount = 2; }
        LOG("[music] %d pursuit song slot(s) from %s", g_songCount, path.c_str());
    }

    static void ApplySong() {
        uint32_t cur = 0;
        if (!Features::On("song")) return;
        if (g_songApplied || !ReadU32(Addr(Ida::ForcedSong), cur)) return;
        int k = static_cast<int>(GetTickCount64() % static_cast<ULONGLONG>(g_songCount));
        if (g_songCount > 1 && g_songs[k] == g_lastSong) k = (k + 1) % g_songCount;
        const int32_t song = g_songs[k];
        g_lastSong = song;
        __try { *reinterpret_cast<volatile int32_t*>(Addr(Ida::ForcedSong)) = song; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        LOG("[music] pursuit song %d", song);
        g_songSaved = cur;
        g_songApplied = true;
    }

    static void ApplySource() {
        uint32_t player = g_player, src = 0;
        if (g_srcApplied || !player || !ReadU32(player + 52, src) || !src) {
            LOG("[music] music source unknown (player 0x%08X) -- not switched", player);
            return;
        }
        __try {
            volatile uint8_t* b = reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(src) + 64);
            g_srcSaved64 = *b;
            *b = 0;
            g_srcApplied = src;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void ApplyPursuit(uint32_t eventKey) {
        LogState("before pursuit");
        ApplyAttrib(eventKey);
        ApplySong();
        ApplySource();
        const ULONGLONG now = GetTickCount64();
        g_diagDueMs[0] = now + 2000;
        g_diagDueMs[1] = now + 6000;
    }

    void Restore() {
        __try {
            if (g_sndApplied)
                for (int i = 0; i < kSndWords; ++i) reinterpret_cast<volatile uint32_t*>(g_sndApplied)[i] = g_sndSaved[i];
            if (g_songApplied) *reinterpret_cast<volatile uint32_t*>(Addr(Ida::ForcedSong)) = g_songSaved;
            if (g_srcApplied) *reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(g_srcApplied) + 64) = g_srcSaved64;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_sndApplied = 0;
        g_songApplied = false;
        g_srcApplied = 0;
        if (g_pulseOffDueMs) { g_pulseOffDueMs = 0; CallMask(false); }
    }

    void Install() {
        LoadSongs();
        g_origPlayerUpdate = Addr32(Ida::MusicPlayerUpdate);
        Hooks::Attach(reinterpret_cast<void**>(&g_origPlayerUpdate), reinterpret_cast<void*>(PlayerUpdateStub), "music player");
    }

    void Remove() {
        Hooks::Detach(reinterpret_cast<void**>(&g_origPlayerUpdate), reinterpret_cast<void*>(PlayerUpdateStub));
    }
}
