// Auto in uso letta dai file del server offline.
//
// Il calore e' per auto (OwnedCarTrans/Heat sul server): cambiando auto in safehouse il server aggiorna subito
// DefaultOwnedCarIndex in drivers\driverN\carslots.xml, quindi all'inizio dell'inseguimento questo modulo sa
// quale auto sta guidando il giocatore e HeatTimer usa il calore salvato per quell'auto.
//
// Cartella del server: prima riga di NFSWorldPursuitProbe_serverdir.txt (assoluta o relativa alla cartella
// dell'ASI), altrimenti ricerca automatica tra le sottocartelle della cartella del gioco e della cartella
// dell'eseguibile: va bene quella con drivers\DefaultPersonaIdx.xml e data\, preferendo quella il cui
// config\config.json (nfswFilePath) punta a questo nfsw.exe. Nessun nome di cartella fisso.
#include "ServerCar.h"
#include "Game.h"
#include "Log.h"
#include <cstdio>
#include <cstring>
#include <cctype>

namespace Mod::ServerCar {
    constexpr uint32_t kCurrentPersonaId = 0x43FDD0;   // unsigned __int64 __cdecl (), persona in uso

    static std::string g_server;      // con '\' finale; vuoto = non ancora trovato
    static bool g_logged = false;

    static bool ReadFile(const std::string& path, std::string& out, FILETIME* mtime = nullptr) {
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        if (mtime) { mtime->dwLowDateTime = mtime->dwHighDateTime = 0; GetFileTime(h, nullptr, nullptr, mtime); }
        DWORD size = GetFileSize(h, nullptr), got = 0;
        bool ok = size != INVALID_FILE_SIZE && size < 4 * 1024 * 1024;
        if (ok) {
            out.resize(size);
            ok = size == 0 || (::ReadFile(h, &out[0], size, &got, nullptr) && got == size);
        }
        CloseHandle(h);
        return ok;
    }
    static bool Exists(const std::string& path) {
        DWORD a = GetFileAttributesA(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES;
    }
    static std::string WithSlash(std::string s) {
        if (!s.empty() && s.back() != '\\' && s.back() != '/') s += '\\';
        return s;
    }
    static std::string Parent(const std::string& dirWithSlash) {
        if (dirWithSlash.size() < 2) return std::string();
        size_t p = dirWithSlash.find_last_of("\\/", dirWithSlash.size() - 2);
        return p == std::string::npos ? std::string() : dirWithSlash.substr(0, p + 1);
    }
    static char Norm(char c) { return c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

    // config\config.json -> "nfswFilePath" e' questo eseguibile?
    static bool ConfigPointsToExe(const std::string& dir, const std::string& exe) {
        std::string js;
        if (!ReadFile(dir + "config\\config.json", js)) return false;
        size_t p = js.find("\"nfswFilePath\"");
        if (p == std::string::npos) return false;
        p = js.find('"', p + 14);
        if (p == std::string::npos) return false;
        std::string v;
        for (++p; p < js.size() && js[p] != '"'; ++p) {
            char c = js[p];
            if (c == '\\' && p + 1 < js.size() && (js[p + 1] == '\\' || js[p + 1] == '/')) c = js[++p];
            v += c;
        }
        if (v.size() != exe.size()) return false;
        for (size_t i = 0; i < v.size(); ++i) if (Norm(v[i]) != Norm(exe[i])) return false;
        return true;
    }

    static int Scan(const std::string& root, const std::string& exe, std::string& best, int bestScore) {
        WIN32_FIND_DATAA fd;
        HANDLE f = FindFirstFileA((root + "*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) return bestScore;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            std::string dir = root + fd.cFileName + "\\";
            if (!Exists(dir + "drivers\\DefaultPersonaIdx.xml") || !Exists(dir + "data")) continue;
            int score = 1 + (ConfigPointsToExe(dir, exe) ? 2 : 0);
            LOG("[servercar] cartella del server candidata: %s%s", dir.c_str(), score >= 3 ? " (configurata per questo nfsw.exe)" : "");
            if (score > bestScore) { bestScore = score; best = dir; }
        } while (FindNextFileA(f, &fd));
        FindClose(f);
        return bestScore;
    }

    static bool FindServer() {
        if (!g_server.empty() && Exists(g_server + "drivers")) return true;
        g_server.clear();
        std::string cfg;
        if (ReadFile(ModuleDir() + "NFSWorldPursuitProbe_serverdir.txt", cfg)) {
            size_t e = cfg.find_first_of("\r\n");
            std::string p = cfg.substr(0, e);
            while (!p.empty() && (p.back() == ' ' || p.back() == '\t')) p.pop_back();
            if (!p.empty()) {
                if (!(p.size() > 1 && (p[1] == ':' || (p[0] == '\\' && p[1] == '\\')))) p = ModuleDir() + p;
                g_server = WithSlash(p);
                LOG("[servercar] cartella del server da NFSWorldPursuitProbe_serverdir.txt: %s", g_server.c_str());
                return Exists(g_server + "drivers");
            }
        }
        char buf[MAX_PATH] = {};
        if (!GetModuleFileNameA(nullptr, buf, MAX_PATH)) return false;
        std::string exe(buf);
        size_t s = exe.find_last_of("\\/");
        if (s == std::string::npos) return false;
        std::string exeDir = exe.substr(0, s + 1), root = Parent(exeDir), best;
        int score = 0;
        if (!root.empty()) score = Scan(root, exe, best, score);
        score = Scan(exeDir, exe, best, score);
        if (score <= 0) {
            if (!g_logged) LOG("[servercar] cartella del server offline non trovata: il calore non puo' essere tenuto per auto");
            g_logged = true;
            return false;
        }
        g_server = best;
        LOG("[servercar] cartella del server: %s", g_server.c_str());
        return true;
    }

    static std::string Tag(const std::string& xml, const char* tag, size_t from = 0, size_t to = std::string::npos) {
        std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
        size_t a = xml.find(open, from);
        if (a == std::string::npos || (to != std::string::npos && a >= to)) return std::string();
        a += open.size();
        size_t b = xml.find(close, a);
        if (b == std::string::npos || (to != std::string::npos && b > to)) return std::string();
        return xml.substr(a, b - a);
    }

    static unsigned PersonaId() {
        unsigned long long id = 0;
        const uint8_t* f = reinterpret_cast<const uint8_t*>(Addr(kCurrentPersonaId));
        if (IsBadReadPtr(f, 1) || f[0] != 0xB8) return 0;
        __try { id = reinterpret_cast<unsigned long long(__cdecl*)()>(Addr(kCurrentPersonaId))(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
        return (id == 0 || id > 0x7FFFFFFFULL) ? 0 : static_cast<unsigned>(id);
    }

    // cartella del pilota in uso: PersonaId uguale a quella del gioco, altrimenti GetPersonaInfo.xml piu' recente
    static std::string DriverDir(unsigned want) {
        std::string drivers = g_server + "drivers\\", newest, match;
        FILETIME best = { 0, 0 };
        WIN32_FIND_DATAA fd;
        HANDLE f = FindFirstFileA((drivers + "driver*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) return std::string();
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::string dir = drivers + fd.cFileName + "\\", info;
            FILETIME t;
            if (!ReadFile(dir + "GetPersonaInfo.xml", info, &t) || !Exists(dir + "carslots.xml")) continue;
            unsigned id = static_cast<unsigned>(strtoul(Tag(info, "PersonaId").c_str(), nullptr, 10));
            if (want && id == want) match = dir;
            if (newest.empty() || CompareFileTime(&t, &best) > 0) { best = t; newest = dir; }
        } while (FindNextFileA(f, &fd));
        FindClose(f);
        return match.empty() ? newest : match;
    }

    Info Current() {
        Info r;
        if (!FindServer()) return r;
        unsigned pid = PersonaId();
        std::string dir = DriverDir(pid), xml;
        if (dir.empty() || !ReadFile(dir + "carslots.xml", xml)) {
            LOG("[servercar] carslots.xml del pilota non trovato (persona %u)", pid);
            return r;
        }
        std::string idxs = Tag(xml, "DefaultOwnedCarIndex");
        if (idxs.empty()) return r;
        int idx = atoi(idxs.c_str());
        size_t pos = 0;
        for (int i = 0; ; ++i) {
            size_t a = xml.find("<OwnedCarTrans>", pos);
            if (a == std::string::npos) return r;
            size_t b = xml.find("</OwnedCarTrans>", a);
            if (b == std::string::npos) return r;
            if (i == idx) {
                // l'Id dell'auto posseduta e' quello fuori da <CustomCar> (dentro c'e' l'Id della personalizzazione)
                size_t cc = xml.find("</CustomCar>", a);
                size_t from = (cc != std::string::npos && cc < b) ? cc : a;
                r.id = Tag(xml, "Id", from, b);
                r.name = Tag(xml, "Name", a, cc != std::string::npos && cc < b ? cc : b);
                std::string h = Tag(xml, "Heat", from, b);
                if (!h.empty()) r.heat = static_cast<float>(atof(h.c_str()));
                r.ok = !r.id.empty();
                LOG("[servercar] persona %u, auto %d: id %s (%s), calore sul server %.2f", pid, idx, r.id.c_str(), r.name.c_str(), r.heat);
                return r;
            }
            pos = b + 16;
        }
    }
}
