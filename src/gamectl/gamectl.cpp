#include "gamectl/gamectl.h"

#include <windows.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "core/gbx.h"
#include "reflect/reflect.h"

namespace gamectl {
namespace {

// What the "Play Track > Solo" path and the race quit button touch, per exe. Found with Ghidra and the
// engine's reflection tables; see tm-modding-knowledge games/tmn-eswc, games/tm-sunrise, games/tmnf, games/tmuf.
struct Spec {
    Game game;
    const char* name;
    uint32_t timestamp;
    uintptr_t app;           // global CGameCtnApp* (stored by its constructor / CGameApp::s_TheGame)
    uintptr_t infos;         // CGameCtnChallengeInfo::s_ChallengeInfos {count, items, capacity}
    uintptr_t refresh;       // CGameCtnChallengeInfo::RefreshFromDisk
    int refreshArgs;         // 3: cdecl(folders*, 0, 0) (ESWC/Sunrise); 2: cdecl(const folders&, nullptr) (Forever)
    bool foldersEmbedded;    // folders object inside the app (Forever) instead of a pointer field
    size_t appFolders;
    uintptr_t sortByName;    // CGameCtnChallengeInfo::SortByName (cdecl, no args)
    uintptr_t gameMalloc, gameFree;
    size_t appChallengeType; // CTrackMania.ChallengeType (reflection name), set to 3 like MenuPlayChallenge_Solo
    size_t appMenus;         // CTrackMania.MenuManager
    size_t menusApp;         // back pointer menus -> app
    size_t menusChosen;
    bool chosenIsBuffer;     // ESWC/Sunrise: {count, items, cap} of chosen infos; Forever: one info pointer
    size_t menusMode;        // 0 = none
    int modeValue;
    size_t menusResult;
    int resultValue;
    size_t appRace;          // current race (CTrackMania.Races in ESWC/Sunrise, SetRace field in Forever)
    uintptr_t quitButton;    // CTrackManiaRace::ButtonQuitOnClick (thiscall, no args)
    const wchar_t* tracksRoot; // L"exe" = <exe dir>\GameData, L"docs" = Documents\TrackMania (Forever default)
};

const Spec kSpecs[] = {
    // TMN ESWC unofficial 1.80, sha256 983bd127...; MenuPlayChallenge_Solo 0x69f8b0, MenuChooseChallenge_OnOk 0x6a0480
    {Game::Eswc, "TrackMania Nations ESWC 1.80", 0x5ce7272f, 0x00caee50, 0x00caed34, 0x00703990, 3, false, 0x1f0,
     0x00703b00, 0x004044b0, 0x004045b0, 0x2e8, 0x104, 0x374, 0x3a4, true, 0x3e4, 0x13, 0x2e4, 0xf, 0x2f0,
     0x006db380, L"exe"},
    // TM Sunrise Win10 repack, sha256 e59dfc7e...; MenuPlayChallenge_Solo 0x7bb660, MenuPlayChallenge_OnOk 0x7bc230
    {Game::Sunrise, "TrackMania Sunrise (Win10 repack)", 0x5cc8f0cf, 0x00bcce50, 0x00bcd1e8, 0x0085fce0, 3, false,
     0x1b8, 0x0085fe50, 0x00402cc0, 0x00402b90, 0x2a0, 0x104, 0x2e8, 0x318, true, 0x354, 2, 0x258, 1, 0x2a8,
     0x007ed580, L"exe"},
    // TMNF (map symbols): CTrackManiaMenus::LaunchManiaCodeAction 0x4fb950 plays a file this way
    // (menus+0x13c = info, SetChallengeType(3), menus+0x704 = 1)
    {Game::Tmnf, "TrackMania Nations Forever", 0x4d431494, 0x00d68c44, 0x00d69120, 0x005cf100, 2, true, 0x148,
     0x005cdde0, 0, 0, 0x418, 0x194, 0x784, 0x13c, false, 0, 0, 0x704, 1, 0x454, 0x0047b290, L"docs"},
    // TMUF: same classes and offsets as TMNF (reflection tables identical), functions ported with fnmatch
    {Game::Tmuf, "TrackMania United Forever", 0x4b9e8add, 0x00d6a2a4, 0x00d6a780, 0x005cf2a0, 2, true, 0x148,
     0x005cdf70, 0, 0, 0x418, 0x194, 0x784, 0x13c, false, 0, 0, 0x704, 1, 0x454, 0x0047b0e0, L"docs"},
};

struct Buffer {
    uint32_t count;
    void** items;
    uint32_t capacity;
};

const Spec* g_spec = nullptr;
Game g_game = Game::Unknown;
LogFn g_log = nullptr;
wchar_t g_lastPath[MAX_PATH] = {};
bool g_experimental = false;
// Image base minus the link base 0x400000: TMLoader's TmForever 2.12.0 is relocated (ASLR), 2.12.0-compat and the
// original exes load at 0x400000 (delta 0). Every link address of a Spec is moved by it.
uintptr_t g_delta = 0;
inline uintptr_t A(uintptr_t link) { return link + g_delta; }

void logf(const char* fmt, ...) {
    if (!g_log) return;
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_log(buf);
}

template <typename T> bool readAt(const void* base, size_t off, T* out) {
    __try {
        *out = *reinterpret_cast<const T*>(static_cast<const char*>(base) + off);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* appPtr() {
    void* app = nullptr;
    if (!g_spec || !readAt(reinterpret_cast<const void*>(A(g_spec->app)), 0, &app)) return nullptr;
    return app;
}

// A field of `obj` (the given offset first, then a scan of the first 0x800 bytes) that holds an engine object
// of the class `kind` (or a subclass).
void* fieldOfKind(void* obj, size_t hint, const char* kind) {
    void* p = nullptr;
    if (hint && readAt(obj, hint, &p) && p && reflect::isKindOf(reflect::classOfObject(p), kind)) return p;
    for (size_t off = 0; off < 0x800; off += 4) {
        if (off == hint) continue;
        if (readAt(obj, off, &p) && p && reinterpret_cast<uintptr_t>(p) > 0x10000 &&
            reflect::isKindOf(reflect::classOfObject(p), kind)) {
            if (hint) logf("gamectl: %s found at +0x%X instead of +0x%X", kind, (unsigned)off, (unsigned)hint);
            return p;
        }
    }
    return nullptr;
}

void* menusPtr(void* app) {
    void* menus = fieldOfKind(app, g_spec ? g_spec->appMenus : 0, "CTrackManiaMenus");
    void* back = nullptr;
    if (menus && g_spec && !(readAt(menus, g_spec->menusApp, &back) && back == app)) {
        logf("gamectl: menus %p does not point back to the app at +0x%X", menus, (unsigned)g_spec->menusApp);
        return nullptr;
    }
    return menus;
}

void* racePtr(void* app) {
    void* race = nullptr;
    // A race field that is null is normal (no race); only scan when the hinted field holds something else.
    if (g_spec && readAt(app, g_spec->appRace, &race) && !race) return nullptr;
    return fieldOfKind(app, g_spec ? g_spec->appRace : 0, "CTrackManiaRace");
}

bool startsWith(const char* s, const char* prefix) { return s && strncmp(s, prefix, strlen(prefix)) == 0; }

bool tracksDir(wchar_t* out, size_t cap) {
    wchar_t base[MAX_PATH] = {};
    const bool docs = g_spec && wcscmp(g_spec->tracksRoot, L"docs") == 0 && !wcsstr(GetCommandLineW(), L"/useexedir");
    if (docs) {
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, base))) return false;
        swprintf_s(out, cap, L"%s\\TrackMania\\Tracks\\Challenges\\Downloaded\\_Horizon\\", base);
    } else {
        DWORD n = GetModuleFileNameW(nullptr, base, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return false;
        wchar_t* slash = wcsrchr(base, L'\\');
        if (!slash) return false;
        slash[1] = 0;
        swprintf_s(out, cap, L"%sGameData\\Tracks\\Challenges\\Downloaded\\_Horizon\\", base);
    }
    return true;
}

bool makeDirs(const wchar_t* dir) {
    wchar_t tmp[MAX_PATH];
    wcscpy_s(tmp, dir);
    for (wchar_t* p = tmp + 3; *p; p++) {
        if (*p == L'\\') {
            *p = 0;
            CreateDirectoryW(tmp, nullptr);
            *p = L'\\';
        }
    }
    return GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES;
}

int refreshAndFindNew(void* app, void** newInfo) {
    __try {
        Buffer* infos = reinterpret_cast<Buffer*>(A(g_spec->infos));
        const uint32_t before = infos->count;
        void** snapshot = static_cast<void**>(HeapAlloc(GetProcessHeap(), 0, (before + 1) * sizeof(void*)));
        if (!snapshot) return -1;
        for (uint32_t i = 0; i < before; i++) snapshot[i] = infos->items[i];
        void* folders = g_spec->foldersEmbedded ? static_cast<char*>(app) + g_spec->appFolders
                                                 : *reinterpret_cast<void**>(static_cast<char*>(app) + g_spec->appFolders);
        if (g_spec->refreshArgs == 3)
            reinterpret_cast<void(__cdecl*)(void*, int, int)>(A(g_spec->refresh))(folders, 0, 0);
        else
            reinterpret_cast<void(__cdecl*)(void*, void*)>(A(g_spec->refresh))(folders, nullptr);
        reinterpret_cast<void(__cdecl*)()>(A(g_spec->sortByName))();
        int added = 0;
        for (uint32_t i = 0; i < infos->count; i++) {
            void* p = infos->items[i];
            bool old = false;
            for (uint32_t j = 0; j < before && !old; j++) old = snapshot[j] == p;
            if (!old) {
                added++;
                *newInfo = p;
            }
        }
        logf("gamectl: challenge infos %u -> %u, %d new", before, infos->count, added);
        HeapFree(GetProcessHeap(), 0, snapshot);
        return added;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("gamectl: exception 0x%08X while rescanning the track folders", GetExceptionCode());
        return -1;
    }
}

bool setMenuToPlay(void* app, void* menus, void* info) {
    __try {
        char* m = static_cast<char*>(menus);
        if (g_spec->chosenIsBuffer) {
            Buffer& chosen = *reinterpret_cast<Buffer*>(m + g_spec->menusChosen);
            if (chosen.capacity < 1 || !chosen.items) {
                void** items = static_cast<void**>(
                    reinterpret_cast<void*(__cdecl*)(size_t)>(A(g_spec->gameMalloc))(4 * sizeof(void*)));
                if (!items) return false;
                if (chosen.items) reinterpret_cast<void(__cdecl*)(void*)>(A(g_spec->gameFree))(chosen.items);
                chosen.items = items;
                chosen.capacity = 4;
            }
            chosen.items[0] = info;
            chosen.count = 1;
        } else {
            *reinterpret_cast<void**>(m + g_spec->menusChosen) = info;
        }
        *reinterpret_cast<int*>(static_cast<char*>(app) + g_spec->appChallengeType) = 3;
        if (g_spec->menusMode) *reinterpret_cast<int*>(m + g_spec->menusMode) = g_spec->modeValue;
        *reinterpret_cast<int*>(m + g_spec->menusResult) = g_spec->resultValue;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("gamectl: exception 0x%08X while setting the menu", GetExceptionCode());
        return false;
    }
}

bool pressQuit(void* race) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(A(g_spec->quitButton))(race);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("gamectl: exception 0x%08X in ButtonQuitOnClick", GetExceptionCode());
        return false;
    }
}

// Unknown exe (e.g. TM Original or another version): log what the reflection finds, so one run is enough
// to fill in a Spec. Changes nothing in the game.
void discover() {
    if (!reflect::ready()) {
        logf("gamectl: discovery: no class registry found");
        return;
    }
    const char* names[] = {"CGameApp", "CGameCtnApp", "CTrackMania", "CTrackManiaMenus", "CTrackManiaRace",
                           "CTrackManiaRace1P", "CGameCtnChallengeInfo", "CGameCtnChallenge"};
    for (const char* n : names) {
        const reflect::ClassInfo* c = reflect::findClass(n);
        logf("gamectl: discovery: class %s %s (id %08X, %u members)", n, c ? "found" : "missing",
             c ? reflect::classId(c) : 0, c ? (unsigned)reflect::paramCount(c) : 0);
    }
    const char* members[][2] = {{"CTrackMania", "ChallengeType"}, {"CTrackMania", "MenuManager"},
                                {"CTrackMania", "Races"},         {"CTrackMania", "Editor"},
                                {"CTrackManiaRace", "Game"},      {"CTrackManiaMenus", "ChallengeInfosAll"}};
    for (auto& m : members)
        logf("gamectl: discovery: %s.%s offset %d", m[0], m[1], reflect::offsetOf(m[0], m[1]));
    const char* methods[][2] = {{"CTrackManiaRace", "ButtonQuitOnClick"},
                                {"CTrackManiaMenus", "MenuPlayChallenge_Solo"},
                                {"CTrackManiaMenus", "MenuChooseChallenge_OnOk"},
                                {"CTrackManiaMenus", "MenuPlayChallenge_OnOk"}};
    for (auto& m : methods) logf("gamectl: discovery: %s::%s at %p", m[0], m[1], reflect::methodOf(m[0], m[1]));
    // App global: a pointer in the exe's writable sections to an object of class CTrackMania.
    auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    int found = 0;
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections && found < 5; s++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        const uint8_t* p = base + sec->VirtualAddress;
        for (size_t i = 0; i + 4 <= sec->Misc.VirtualSize && found < 5; i += 4) {
            void* obj = nullptr;
            if (!readAt(p, i, &obj) || reinterpret_cast<uintptr_t>(obj) < 0x10000) continue;
            if (reflect::isKindOf(reflect::classOfObject(obj), "CTrackMania")) {
                logf("gamectl: discovery: app candidate global %p -> %p", p + i, obj);
                found++;
            }
        }
    }
}

} // namespace

Game init(LogFn log, bool allowExperimentalOriginal) {
    g_log = log;
    g_experimental = allowExperimentalOriginal;
    auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const uint32_t ts = nt->FileHeader.TimeDateStamp;
    g_spec = nullptr;
    g_delta = reinterpret_cast<uintptr_t>(base) - 0x400000;
    for (const Spec& s : kSpecs)
        if (s.timestamp == ts) g_spec = &s;
    const bool reflection = reflect::init(ts);
    if (g_spec) {
        g_game = g_spec->game;
        logf("gamectl: %s (PE timestamp %08X, image base %p%s), reflection %s", g_spec->name, ts, base,
             g_delta ? ", relocated: link addresses moved" : "", reflection ? "ok" : "missing");
        return g_game;
    }
    g_game = Game::Unknown;
    logf("gamectl: unknown exe (PE timestamp %08X), reflection %s - play/quit disabled%s", ts,
         reflection ? "found by pattern" : "missing", allowExperimentalOriginal ? ", running discovery" : "");
    if (allowExperimentalOriginal) {
        discover();
        // TM Original would be identified here once a run of the discovery has given its Spec values.
        if (reflect::findClass("CTrackMania")) g_game = Game::Original;
    }
    return g_game;
}

Game game() { return g_game; }

void* appObject() { return appPtr(); }

const char* gameName() {
    if (g_spec) return g_spec->name;
    return g_game == Game::Original ? "TrackMania Original (experimental, not supported yet)" : "unknown";
}

State state() {
    void* app = appPtr();
    if (!app) return State::Loading;
    if (void* race = racePtr(app)) {
        const reflect::ClassInfo* c = reflect::classOfObject(race);
        const char* name = reflect::className(c);
        if (reflect::isKindOf(c, "CTrackManiaRace1P")) return State::SoloRace;
        if (name && strstr(name, "Net")) return State::NetRace;
        if (name && strstr(name, "Editor")) return State::Editor;
        return State::Unknown;
    }
    int editorOff = reflect::offsetOf("CTrackMania", "Editor");
    void* editor = nullptr;
    if (editorOff >= 0 && readAt(app, editorOff, &editor) && editor) return State::Editor;
    return menusPtr(app) ? State::MainMenu : State::Loading;
}

bool isOnline() { return state() == State::NetRace; }

Result playMapFile(const wchar_t* path) {
    if (!g_spec) return Result::NotSupported;
    if (state() != State::MainMenu) return Result::WrongState;
    void* app = appPtr();
    void* menus = app ? menusPtr(app) : nullptr;
    if (!menus) return Result::WrongState;
    wchar_t dir[MAX_PATH], dst[MAX_PATH];
    if (!tracksDir(dir, MAX_PATH) || !makeDirs(dir)) {
        logf("gamectl: cannot create the tracks folder");
        return Result::FileError;
    }
    swprintf_s(dst, L"%sHZ_%010lu.Challenge.Gbx", dir, GetTickCount());
    if (!CopyFileW(path, dst, FALSE)) {
        logf("gamectl: copy failed (%lu)", GetLastError());
        return Result::FileError;
    }
    void* info = nullptr;
    int added = refreshAndFindNew(app, &info);
    if (added < 0) return Result::GameError;
    if (added != 1 || !info) {
        logf("gamectl: expected 1 new map in the game's list, got %d (tracks folder %ls)", added, dir);
        return Result::NotFound;
    }
    if (!setMenuToPlay(app, menus, info)) return Result::GameError;
    wcscpy_s(g_lastPath, dst);
    logf("gamectl: playing %ls", dst);
    return Result::Ok;
}

Result returnToMenu() {
    if (!g_spec) return Result::NotSupported;
    if (state() != State::SoloRace) return Result::WrongState;
    void* app = appPtr();
    void* race = app ? racePtr(app) : nullptr;
    if (!race) return Result::WrongState;
    return pressQuit(race) ? Result::Ok : Result::GameError;
}

bool currentMap(MapInfo* out) {
    if (!out || !g_lastPath[0]) return false;
    char utf8[MAX_PATH * 3];
    if (!WideCharToMultiByte(CP_UTF8, 0, g_lastPath, -1, utf8, sizeof(utf8), nullptr, nullptr)) return false;
    lr::GbxMapHeader h = lr::readChallengeHeaderFile(utf8);
    if (!h.ok) return false;
    memset(out, 0, sizeof(*out));
    strncpy_s(out->uid, h.uid.c_str(), _TRUNCATE);
    strncpy_s(out->name, h.name.c_str(), _TRUNCATE);
    strncpy_s(out->author, h.author.c_str(), _TRUNCATE);
    out->authorTime = h.authorMs > 0 ? h.authorMs : -1;
    out->goldTime = h.goldMs > 0 ? h.goldMs : -1;
    out->silverTime = h.silverMs > 0 ? h.silverMs : -1;
    out->bronzeTime = h.bronzeMs > 0 ? h.bronzeMs : -1;
    wcscpy_s(out->path, g_lastPath);
    return true;
}

void cleanupCopies(int keep) {
    wchar_t dir[MAX_PATH], pat[MAX_PATH];
    if (!tracksDir(dir, MAX_PATH)) return;
    swprintf_s(pat, L"%sHZ_*.Challenge.Gbx", dir);
    // names carry a zero-padded tick count, so alphabetical order is age order
    wchar_t names[256][64];
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (n < 256 && wcslen(fd.cFileName) < 64) wcscpy_s(names[n++], fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    qsort(names, n, sizeof(names[0]), [](const void* a, const void* b) {
        return wcscmp(static_cast<const wchar_t*>(a), static_cast<const wchar_t*>(b));
    });
    for (int i = 0; i < n - keep; i++) {
        wchar_t full[MAX_PATH];
        swprintf_s(full, L"%s%s", dir, names[i]);
        if (_wcsicmp(full, g_lastPath) != 0) DeleteFileW(full);
    }
}

} // namespace gamectl
