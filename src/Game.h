#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace Mod {

    // Every nfsw.exe address below is an IDA VA (image base 0x400000);
    // Addr() rebases it on the real module base.
    namespace Ida {
        // cops / player
        constexpr uint32_t CopMgrPtr          = 0xD1EEE0; // ICopMgr*: vt+44 nearest cop, +0x1E0 AIPursuit
        constexpr uint32_t LocalPlayerBase    = 0xD1FA88; // local player array (entry 0 = us)
        constexpr uint32_t LocalPlayerCount   = 0xD1FA90;
        constexpr uint32_t LocalPlayerHolder  = 0xD2037C; // (*(*this))->vt[1]() = player ISimable
        constexpr uint32_t QueryInterface     = 0x7536B0; // thiscall(ifaceTable, keyFn)
        constexpr uint32_t KeyICollisionBody  = 0x6519F0;
        constexpr uint32_t KeyIPursuitAI      = 0x6D0E40;
        constexpr uint32_t CopId              = 0x87F360; // (cop) -> id for ICopMgr::Adopt
        constexpr uint32_t StartPursuitAI     = 0x7FEC60; // IPursuitAI::StartPursuit(this, 0, simable)
        constexpr uint32_t CollisionActivate  = 0x812600; // CSTATE_Collision activate (fires on real contact)
        // top-level UI/flow FSM
        constexpr uint32_t Fsm                = 0xCE62CC; // +0 index (1 freeroam, 4 pursuit), +0x24 init guard
        constexpr uint32_t FsmSetState        = 0x49DB10; // thiscall(fsm, 0, index)
        constexpr uint32_t ExitPursuitMode    = 0x438A50; // game calls it when the results screen closes
        constexpr uint32_t CurrentScreen      = 0x52DA90;
        constexpr uint32_t ModeCtx            = 0xCECC8C; // +0xF0 = mode object read by ModeGetter
        constexpr uint32_t ModeGetter         = 0x68ED10; // 12 = pursuit
        // HUD / gadgets / event settings
        constexpr uint32_t SetGadgetVisible   = 0x532380; // stdcall(name, show)
        constexpr uint32_t EventSettings      = 0xD11948; // 16-byte object: 4 Attrib keys (class 40595996)
        constexpr uint32_t PowerupMgr         = 0xCE62A8; // +168 powerup page
        constexpr uint32_t SetPowerupPage     = 0x532510; // stdcall(page), what GameCore calls
        constexpr uint32_t HashName           = 0x473000;
        constexpr uint32_t GadgetLookup       = 0x4836B0; // ECX = CurrentScreen+40, arg = hash
        constexpr uint32_t GadgetRefreshPage  = 0x48A3B0; // thiscall(powerup gadget)
        // music
        constexpr uint32_t SoundMgr           = 0xD77CC4; // snd Attrib instance = *(*this + 200)
        constexpr uint32_t MusicMask          = 0x77DB00; // cdecl(on, bit)
        constexpr uint32_t AttribCollection   = 0x6C1D10; // (classKey, key) -> collection
        constexpr uint32_t ForcedSong         = 0xC850CC; // >= 0 forces the pursuit song id
        constexpr uint32_t MusicPlayerUpdate  = 0x83BA90; // ECX = music player, source = *(player+52)
        // results screen
        constexpr uint32_t ArbPacketHandler   = 0x51E610; // Flash HandleRequest_Interop_ArbitratedPacket
        constexpr uint32_t ArbRequestCtor     = 0x51E120; // thiscall(req, sink, callbackName)
        constexpr uint32_t ArbRequestVtable   = 0xB80FF0;
        constexpr uint32_t ArbAnswer          = 0x51B0E0; // thiscall(req, packet)
        constexpr uint32_t NewPursuitPacket   = 0x69F870; // cdecl() -> PursuitArbitratedPacket (entrant at +64)
        constexpr uint32_t EntrantSerialize   = 0x499470; // ECX = entrant, called for every results screen
        constexpr uint32_t CarDataSerialize   = 0x4973C0; // ECX = CarData (entrant+0xE8), CarData vt[1]
        constexpr uint32_t StringAssign       = 0x403D00; // thiscall(std::string*, begin, end)
        constexpr uint32_t EmptyString        = 0xCF74F0;
        constexpr uint32_t RatingVtable       = 0xB25464; // HAT rating {vt, float n/1000, int n, string "n"}
    }

    // gameplay.native.dll (IDA base 0x10000000): GameCore.Cops.LaunchPursuit(int heat, bool spawnCar)
    namespace GameplayNative {
        constexpr const char* Module          = "gameplay.native.dll";
        constexpr uint32_t IdaBase            = 0x10000000;
        constexpr uint32_t LaunchPursuitSlot  = 0x102787D4; // EASharp slot, cdecl; holds the binding stub
                                                            // (sub_101EF560) until first use -- same signature
    }

    uintptr_t ExeBase();
    inline uintptr_t Addr(uint32_t idaVa) { return ExeBase() + (idaVa - 0x400000); }
    inline uint32_t Addr32(uint32_t idaVa) { return static_cast<uint32_t>(Addr(idaVa)); }

    bool ReadU32(uintptr_t address, uint32_t& out);
    uint32_t ReadU32Or(uintptr_t address, uint32_t fallback);

    // Call with ECX set AND the args pushed, ESP restored afterwards: works for
    // cdecl, stdcall and thiscall alike (used where the convention is unclear).
    uint32_t CallAny1(uint32_t fn, uint32_t ecx, uint32_t a1);
    uint32_t CallAny2(uint32_t fn, uint32_t ecx, uint32_t a1, uint32_t a2);
    bool TryCallAny1(uint32_t fn, uint32_t ecx, uint32_t a1, uint32_t& out);

    // The game thread is the one owning the main window; game calls are only
    // made from there (see Hooks: QueryInterface pump).
    void ResolveGameThread();
    bool OnGameThread();
    bool GameHasFocus();

    std::string ModuleDir(); // folder of this .asi, with trailing slash
}
