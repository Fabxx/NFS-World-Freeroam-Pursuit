// NFSWorldPursuitProbe_disable.txt: one feature name per line turns it off.
#include "Features.h"
#include "Game.h"
#include "Log.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace Mod::Features {
    static char g_off[32][24] = {};
    static int g_offCount = 0;

    void Load() {
        FILE* f = nullptr;
        std::string path = ModuleDir() + "NFSWorldPursuitProbe_disable.txt";
        if (fopen_s(&f, path.c_str(), "r") != 0 || !f) return;
        char line[64];
        while (fgets(line, sizeof(line), f) && g_offCount < 32) {
            char name[24] = {};
            if (sscanf_s(line, "%23s", name, static_cast<unsigned>(sizeof(name))) != 1 || name[0] == '#') continue;
            strcpy_s(g_off[g_offCount++], name);
            LOG("[features] OFF: %s", name);
        }
        fclose(f);
    }

    bool On(const char* name) {
        for (int i = 0; i < g_offCount; ++i) if (_stricmp(g_off[i], name) == 0) return false;
        return true;
    }
}
