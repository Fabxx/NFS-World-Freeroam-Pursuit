#include "Music.h"
#include "Game.h"
#include "Hooks.h"
#include "Log.h"
#include <cstring>

namespace Mod::Music {

    constexpr char kMaskBit = 20;
    constexpr ULONGLONG kPulseMs = 400;
    constexpr uint32_t kEventSettingsClass = 40595996;
    constexpr int32_t kPursuitSong = 1;
    constexpr int kSndWords = 8;

    static ULONGLONG g_pulseOffDueMs = 0;   // game thread only

    // Music player = ECX of its update method; captured by a register-only
    // stub (no assumption about the method's signature).
    static uint32_t g_origPlayerUpdate = 0;
    static volatile uint32_t g_player = 0;

    static __declspec(naked) void PlayerUpdateStub() {
        __asm {
            mov  g_player, ecx
            jmp  dword ptr [g_origPlayerUpdate]
        }
    }

    static uint32_t g_sndApplied = 0;       // snd instance we wrote, 0 = none
    static uint32_t g_sndSaved[kSndWords] = {};
    static bool g_songApplied = false;
    static uint32_t g_songSaved = 0;
    static uint32_t g_srcApplied = 0;       // music source we wrote, 0 = none
    static uint8_t g_srcSaved64 = 0;

    static void CallMask(bool on) {
        __try { reinterpret_cast<void(__cdecl*)(char, char)>(Addr(Ida::MusicMask))(on ? 1 : 0, kMaskBit); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void BeginPulse(ULONGLONG now) {
        CallMask(true);
        g_pulseOffDueMs = now + kPulseMs;
    }

    void Tick(ULONGLONG now) {
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
        // Only overwrite a real Attrib instance: word 0 = collection, word 1 = its layout (*(coll+28)).
        // Sometimes mgr+200 holds another object (word 0 = vtable): writing it crashed 3 ms later
        // in the tick list sub_654AC0 (call into coll+0x570).
        const uint32_t mb = static_cast<uint32_t>(ExeBase());
        uint32_t curLayout = 0;
        bool isImage = cur[0] >= mb && cur[0] < mb + 0x1000000;
        if (isImage || !cur[0] || !ReadU32(cur[0] + 28, curLayout) || curLayout != cur[1]) {
            LOG("[music] snd 0x%08X is not an Attrib instance now (%08X %08X) -- pursuit music not applied", snd, cur[0], cur[1]);
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
        LOG("[music] snd 0x%08X -> event collection 0x%08X (layout 0x%08X)", snd, coll, layout);
    }

    static void ApplySong() {
        uint32_t cur = 0;
        if (g_songApplied || !ReadU32(Addr(Ida::ForcedSong), cur)) return;
        __try { *reinterpret_cast<volatile int32_t*>(Addr(Ida::ForcedSong)) = kPursuitSong; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }
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
        ApplyAttrib(eventKey);
        ApplySong();
        ApplySource();
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
        g_origPlayerUpdate = Addr32(Ida::MusicPlayerUpdate);
        Hooks::Attach(reinterpret_cast<void**>(&g_origPlayerUpdate), reinterpret_cast<void*>(PlayerUpdateStub), "music player");
    }

    void Remove() {
        Hooks::Detach(reinterpret_cast<void**>(&g_origPlayerUpdate), reinterpret_cast<void*>(PlayerUpdateStub));
    }
}
