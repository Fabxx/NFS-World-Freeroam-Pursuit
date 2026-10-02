#pragma once

namespace Mod::MarkerFx {
    void Install();
    void Remove();
    void Hide();
    void Tick();
    int FlagSpotsTurnedOnBy(void (*fn)(void*), void* ctx);
    void Restore();
}
