#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace Mod {
    namespace Ida {
        constexpr uint32_t CopMgrPtr          = 0xD1EEE0;
        constexpr uint32_t LocalPlayerBase    = 0xD1FA88;
        constexpr uint32_t LocalPlayerCount   = 0xD1FA90;
        constexpr uint32_t LocalPlayerHolder  = 0xD2037C;
        constexpr uint32_t QueryInterface     = 0x7536B0;
        constexpr uint32_t KeyICollisionBody  = 0x6519F0;
        constexpr uint32_t KeyIPursuitAI      = 0x6D0E40;
        constexpr uint32_t KeyIPerpetrator    = 0x65D6A0;
        constexpr uint32_t CopId              = 0x87F360;
        constexpr uint32_t StartPursuitAI     = 0x7FEC60;
        constexpr uint32_t CollisionActivate  = 0x812600;
        constexpr uint32_t Fsm                = 0xCE62CC;
        constexpr uint32_t FsmSetState        = 0x49DB10;
        constexpr uint32_t ExitPursuitMode    = 0x438A50;
        constexpr uint32_t CurrentScreen      = 0x52DA90;
        constexpr uint32_t ModeCtx            = 0xCECC8C;
        constexpr uint32_t ModeGetter         = 0x68ED10;
        constexpr uint32_t SetGadgetVisible   = 0x532380;
        constexpr uint32_t EventSettings      = 0xD11948;
        constexpr uint32_t PowerupMgr         = 0xCE62A8;
        constexpr uint32_t SetPowerupPage     = 0x532510;
        constexpr uint32_t HashName           = 0x473000;
        constexpr uint32_t GadgetLookup       = 0x4836B0;
        constexpr uint32_t GadgetRefreshPage  = 0x48A3B0;
        constexpr uint32_t SoundMgr           = 0xD77CC4;
        constexpr uint32_t MusicMask          = 0x77DB00;
        constexpr uint32_t AttribCollection   = 0x6C1D10;
        constexpr uint32_t ForcedSong         = 0xC850CC;
        constexpr uint32_t MusicPlayerUpdate  = 0x83BA90;
        constexpr uint32_t ArbPacketHandler   = 0x51E610;
        constexpr uint32_t ArbRequestCtor     = 0x51E120;
        constexpr uint32_t ArbRequestVtable   = 0xB80FF0;
        constexpr uint32_t ArbAnswer          = 0x51B0E0;
        constexpr uint32_t NewPursuitPacket   = 0x69F870;
        constexpr uint32_t EntrantSerialize   = 0x499470;
        constexpr uint32_t CarDataSerialize   = 0x4973C0;
        constexpr uint32_t StringAssign       = 0x403D00;
        constexpr uint32_t EmptyString        = 0xCF74F0;
        constexpr uint32_t RatingVtable       = 0xB25464;
    }

    namespace GameplayNative {
        constexpr const char* Module          = "gameplay.native.dll";
        constexpr uint32_t IdaBase            = 0x10000000;
        constexpr uint32_t LaunchPursuitSlot  = 0x102787D4;
    }

    uintptr_t ExeBase();
    inline uintptr_t Addr(uint32_t idaVa) { return ExeBase() + (idaVa - 0x400000); }
    inline uint32_t Addr32(uint32_t idaVa) { return static_cast<uint32_t>(Addr(idaVa)); }

    bool ReadU32(uintptr_t address, uint32_t& out);
    uint32_t ReadU32Or(uintptr_t address, uint32_t fallback);

    uint32_t CallAny1(uint32_t fn, uint32_t ecx, uint32_t a1);
    uint32_t CallAny2(uint32_t fn, uint32_t ecx, uint32_t a1, uint32_t a2);
    bool TryCallAny1(uint32_t fn, uint32_t ecx, uint32_t a1, uint32_t& out);

    void ResolveGameThread();
    bool OnGameThread();
    bool GameHasFocus();

    std::string ModuleDir();
}
