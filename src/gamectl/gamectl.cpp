#include "gamectl/gamectl.h"

#include <windows.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <vector>

#include "core/gbx.h"
#include "gamectl/gamectl_pick.h"
#include "reflect/reflect.h"

namespace gamectl {
namespace {

// What the "Play Track > Solo" path and the race quit button touch, per exe. Found with Ghidra and the
// engine's reflection tables; see tm-modding-knowledge games/tmn-eswc, games/tm-sunrise, games/tmnf, games/tmuf.
//
// Solo play on ESWC/Sunrise/Original (the game's own path, checked statically 2026-11-12): Play > "Play Track"
// (CTrackManiaMenus::MenuPlayChallenge_Solo) sets ChallengeType 3 and the menu mode, then opens the track browser.
// Activating a map there (ESWC 0x6a01e0, Sunrise 0x7bbf90, Original 0x7baaa0, the browser's event-3 handler) stores
// the info in SelectedChallengeInfo and sets the menu result to 1. The menu fiber (ESWC 0x7887b0, Sunrise 0x87b060)
// returns that result to the app loop (ESWC 0x6e65f0, Sunrise 0x800070); result 1 starts the play fiber (ESWC
// 0x6eb540), which loads SelectedChallengeInfo's file (info+0x14, class 0x24003000). The result is read the same
// way on every menu page, so the main menu works too. The chosen-infos buffer is only read for ChallengeType 10
// (playlists), and MenuChooseChallenge_OnOk's ESWC result 0xf (ChallengeType not 10/11) is not handled by the app
// loop at all: it goes back to the menu and reopens the page of the menu mode (0x13 = the track browser).
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
    size_t appChallengeType; // CTrackMania.ChallengeType (reflection name), set to 3 like MenuPlayChallenge_Solo
    size_t appMenus;         // CTrackMania.MenuManager
    size_t menusApp;         // back pointer menus -> app
    size_t menusSelected;    // CTrackManiaMenus.SelectedChallengeInfo (Forever: the info LaunchManiaCodeAction sets)
    size_t menusMode;        // menu mode = page shown when the menu fiber starts again; 0 = none
    int modeValue;
    size_t menusResult;      // menu result the menu fiber hands to the app loop
    int resultValue;         // 1 = play SelectedChallengeInfo
    size_t appRace;          // current race (CTrackMania.Races in ESWC/Sunrise, SetRace field in Forever)
    uintptr_t quitButton;    // CTrackManiaRace::ButtonQuitOnClick (thiscall, no args)
    const wchar_t* tracksRoot; // L"exe" = <exe dir>\GameData, L"docs" = Documents\TrackMania (Forever default)
};

const Spec kSpecs[] = {
    // TMN ESWC unofficial 1.80, sha256 983bd127...; MenuPlayChallenge_Solo 0x69f8b0 (mode 0x13), browser play 0x6a01e0
    {Game::Eswc, "TrackMania Nations ESWC 1.80", 0x5ce7272f, 0x00caee50, 0x00caed34, 0x00703990, 3, false, 0x1f0,
     0x00703b00, 0x2e8, 0x104, 0x374, 0x47c, 0x3e4, 0x13, 0x2e4, 1, 0x2f0, 0x006db380, L"exe"},
    // TM Sunrise Win10 repack, sha256 e59dfc7e...; MenuPlayChallenge_Solo 0x7bb660 (mode 2), browser play 0x7bbf90
    {Game::Sunrise, "TrackMania Sunrise (Win10 repack)", 0x5cc8f0cf, 0x00bcce50, 0x00bcd1e8, 0x0085fce0, 3, false,
     0x1b8, 0x0085fe50, 0x2a0, 0x104, 0x2e8, 0x3d4, 0x354, 2, 0x258, 1, 0x2a8, 0x007ed580, L"exe"},
    // TM Original, Online Patch v2 community exe, sha256 75e129d7...: Sunrise's code with the same menu, app and race
    // offsets (compared instruction by instruction); MenuPlayChallenge_Solo 0x7ba170, browser play 0x7baaa0,
    // SetChallengeType 0x823370 (trackmania-internals games/tm-original). Static, not run yet.
    {Game::Original, "TrackMania Original (Online Patch v2)", 0x5d337477, 0x00bc8084, 0x00bc841c, 0x0085e0a0, 3, false,
     0x1b8, 0x0085e210, 0x2a0, 0x104, 0x2e8, 0x3d4, 0x354, 2, 0x258, 1, 0x2a8, 0x007ebb00, L"exe"},
    // TMNF (map symbols): CTrackManiaMenus::LaunchManiaCodeAction 0x4fb950 plays a file this way
    // (menus+0x13c = info, SetChallengeType(3), menus+0x704 = 1). The host plays Forever maps through
    // src/host/game/forever.cpp now; the fields stay for state() and returnToMenu().
    {Game::Tmnf, "TrackMania Nations Forever", 0x4d431494, 0x00d68c44, 0x00d69120, 0x005cf100, 2, true, 0x148,
     0x005cdde0, 0x418, 0x194, 0x784, 0x13c, 0, 0, 0x704, 1, 0x454, 0x0047b290, L"docs"},
    // TMUF: same classes and offsets as TMNF (reflection tables identical), functions ported with fnmatch
    {Game::Tmuf, "TrackMania United Forever", 0x4b9e8add, 0x00d6a2a4, 0x00d6a780, 0x005cf2a0, 2, true, 0x148,
     0x005cdf70, 0x418, 0x194, 0x784, 0x13c, 0, 0, 0x704, 1, 0x454, 0x0047b0e0, L"docs"},
};

// CConstructionChallengeInfo+0x14: the info's CSystemFidFile (Build 0x702a60 / 0x85ede0 / 0x85d1a0 look infos up by
// it, RefreshFromDisk drops infos whose file is gone, the play fiber loads it). Same in ESWC, Sunrise, Original.
constexpr size_t kInfoFid = 0x14;
// CSystemFidFile.FileName {u32 length, char* text} (reflection offset 88 in ESWC and Sunrise; the same {length,
// text} layout as the info's Password at +0x6c that the play fiber reads).
constexpr int kFidFileNameDefault = 0x58;
// CConstructionChallengeInfo.Kind (reflection, logged only; 7 survives a rescan without a file).
constexpr int kInfoKindDefault = 0x60;

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

// The info list as raw pointers (SEH-guarded: the game owns the buffer). -1 on an exception.
int infoCount() {
    __try {
        return static_cast<int>(reinterpret_cast<Buffer*>(A(g_spec->infos))->count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
int copyInfos(void** out, uint32_t cap) {
    __try {
        Buffer* infos = reinterpret_cast<Buffer*>(A(g_spec->infos));
        const uint32_t n = infos->count < cap ? infos->count : cap;
        for (uint32_t i = 0; i < n; i++) out[i] = infos->items[i];
        return static_cast<int>(n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
bool snapshotInfos(std::vector<void*>& v) {
    const int n = infoCount();
    if (n < 0 || n > 1000000) return false;
    v.assign(static_cast<size_t>(n) + 64, nullptr);
    const int got = copyInfos(v.data(), static_cast<uint32_t>(v.size()));
    if (got < 0) return false;
    v.resize(static_cast<size_t>(got));
    return true;
}

// RefreshFromDisk + SortByName, the game's own rescan of its track folders. One call per play_map_file.
bool refreshInfos(void* app) {
    __try {
        void* folders = g_spec->foldersEmbedded ? static_cast<char*>(app) + g_spec->appFolders
                                                 : *reinterpret_cast<void**>(static_cast<char*>(app) + g_spec->appFolders);
        if (g_spec->refreshArgs == 3)
            reinterpret_cast<void(__cdecl*)(void*, int, int)>(A(g_spec->refresh))(folders, 0, 0);
        else
            reinterpret_cast<void(__cdecl*)(void*, void*)>(A(g_spec->refresh))(folders, nullptr);
        reinterpret_cast<void(__cdecl*)()>(A(g_spec->sortByName))();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("gamectl: exception 0x%08X while rescanning the track folders", GetExceptionCode());
        return false;
    }
}

// Offset of an info's gold time (ms): reflection ("GoldTime" of CConstructionChallengeInfo on ESWC/Sunrise/
// Original, CGameCtnChallengeInfo on Forever), else the dumped offsets 88 / 100.
int goldOffset() {
    int off = reflect::offsetOf("CConstructionChallengeInfo", "GoldTime");
    if (off < 0) off = reflect::offsetOf("CGameCtnChallengeInfo", "GoldTime");
    if (off < 0 && g_spec) off = (g_spec->game == Game::Tmnf || g_spec->game == Game::Tmuf) ? 100 : 88;
    return off;
}

// Offsets read from the reflection when it has them (the static table is the fallback).
int fidFileNameOffset() {
    const int off = reflect::offsetOf("CSystemFidFile", "FileName");
    return off >= 0 ? off : kFidFileNameDefault;
}
int infoKindOffset() {
    const int off = reflect::offsetOf("CConstructionChallengeInfo", "Kind");
    return off >= 0 ? off : kInfoKindDefault;
}

// File name of an info's CSystemFidFile (info+0x14 -> FileName). False if the info has no file, the pointer is not
// a CSystemFidFile or the text is unreadable. Read only, SEH-guarded.
bool copyText(const char* src, uint32_t len, char* out) {
    __try {
        memcpy(out, src, len);
        out[len] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// CSystemFidFile.FileName is a wide string (reflection type 0x29, the type of CConstructionChallenge.ChallengeName,
// which the host reads as {length, wchar_t*}; ESWC 1.80 member entry 0x00c08588). Read as 8-bit text it gave the
// first letter only ("H"): no info ever matched the copy's name ("176 of 205 infos have a readable file name, 0 match",
// ESWC log 2026-11-12). Read wide; a text that is not UTF-16 (no zero high bytes) is taken as 8-bit.
bool copyWide(const wchar_t* src, uint32_t len, wchar_t* out) {
    __try {
        memcpy(out, src, len * sizeof(wchar_t));
        out[len] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool infoFileName(void* info, int nameOff, char* out, size_t cap) {
    void* fid = nullptr;
    if (!info || !readAt(info, kInfoFid, &fid) || reinterpret_cast<uintptr_t>(fid) < 0x10000) return false;
    if (reflect::ready() && !reflect::isKindOf(reflect::classOfObject(fid), "CSystemFidFile")) return false;
    uint32_t len = 0;
    const void* text = nullptr;
    if (!readAt(fid, static_cast<size_t>(nameOff), &len) || !readAt(fid, static_cast<size_t>(nameOff) + 4, &text)) return false;
    if (!text || len == 0 || len >= cap || len >= MAX_PATH) return false;
    wchar_t wide[MAX_PATH + 1];
    if (copyWide(static_cast<const wchar_t*>(text), len, wide)) {
        // 8-bit text read as UTF-16 packs two letters per unit (>= 0x2020); a file name's first letter is < 0x100
        bool utf16 = wide[0] < 0x100;
        for (uint32_t i = 0; i < len && utf16; i++)
            if (wide[i] == 0) utf16 = false;
        if (utf16) {
            const int n = WideCharToMultiByte(CP_ACP, 0, wide, static_cast<int>(len), out, static_cast<int>(cap) - 1, nullptr, nullptr);
            if (n > 0) {
                out[n] = 0;
                return true;
            }
        }
    }
    return copyText(static_cast<const char*>(text), len, out);
}

// The game's own "play the selected map" (see the comment above kSpecs): SelectedChallengeInfo = info,
// ChallengeType 3 (solo), menu mode as MenuPlayChallenge_Solo sets it, menu result 1. The menu fiber hands the
// result to the app loop within the next frames.
bool setMenuToPlay(void* app, void* menus, void* info) {
    __try {
        char* m = static_cast<char*>(menus);
        *reinterpret_cast<void**>(m + g_spec->menusSelected) = info;
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

// Unknown exe (another version of a known game): log what the reflection finds, so one run is enough
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
        // An unknown exe with the TrackMania app class (e.g. another TM Original build): reported, never controlled.
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
    // A result the menu fiber has not handed on yet (a button pressed this frame): not ours to overwrite.
    int pending = 0;
    if (!readAt(menus, g_spec->menusResult, &pending)) return Result::GameError;
    if (pending != 0) {
        logf("gamectl: the menu is busy (result %d pending), not starting a map", pending);
        return Result::WrongState;
    }
    // The selected-info field must be where the reflection says (the game reads it to load the map).
    const int selOff = reflect::offsetOf("CTrackManiaMenus", "SelectedChallengeInfo");
    if (selOff >= 0 && static_cast<size_t>(selOff) != g_spec->menusSelected) {
        logf("gamectl: CTrackManiaMenus.SelectedChallengeInfo is at +0x%X, expected +0x%X - not starting a map", selOff,
             (unsigned)g_spec->menusSelected);
        return Result::GameError;
    }
    wchar_t dir[MAX_PATH], dst[MAX_PATH];
    if (!tracksDir(dir, MAX_PATH) || !makeDirs(dir)) {
        logf("gamectl: cannot create the tracks folder");
        return Result::FileError;
    }
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        logf("gamectl: map file not found (%lu)", GetLastError());
        return Result::FileError;
    }
    // The source may be anywhere (a plugin's data folder, a download folder): it is copied into the one folder
    // the game scans under a unique name and found again by that file name (gamectl_pick.h says why pointers and
    // counts are no proof).
    char src8[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, path, -1, src8, sizeof(src8), nullptr, nullptr);
    const lr::GbxMapHeader hdr = lr::readChallengeHeaderFile(src8);
    if (!hdr.ok) {
        logf("gamectl: not a readable challenge file (%s)", hdr.error.c_str());
        return Result::FileError;
    }
    std::vector<void*> before, after;
    if (!snapshotInfos(before)) return Result::GameError;
    swprintf_s(dst, L"%sHZ_%010lu.Challenge.Gbx", dir, GetTickCount());
    if (!CopyFileW(path, dst, FALSE)) {
        logf("gamectl: copy failed (%lu)", GetLastError());
        return Result::FileError;
    }
    const DWORD t0 = GetTickCount();
    if (!refreshInfos(app) || !snapshotInfos(after)) return Result::GameError;
    const int goff = goldOffset();
    const int nameOff = fidFileNameOffset();
    const wchar_t* slash = wcsrchr(dst, L'\\');
    char want[MAX_PATH] = {};
    WideCharToMultiByte(CP_ACP, 0, slash ? slash + 1 : dst, -1, want, sizeof(want), nullptr, nullptr);
    PickReport rep;
    const int pick = pickCopiedChallenge(
        before, after, [nameOff](void* info, char* buf, size_t cap) { return infoFileName(info, nameOff, buf, cap); }, want,
        [goff](void* info, int* ms) { return goff >= 0 && readAt(info, static_cast<size_t>(goff), ms); }, hdr.goldMs, &rep);
    logf("gamectl: challenge infos %u -> %u (%d new pointers, rescan %lu ms); %s %s: %d of %u infos have a readable file "
         "name (FileName +0x%X), %d match",
         static_cast<unsigned>(before.size()), static_cast<unsigned>(after.size()), rep.newCount, GetTickCount() - t0,
         rep.method == PickMethod::FileName ? "looking for the file" : "no file names readable, falling back to the gold time of",
         rep.method == PickMethod::FileName ? want : "the new infos", rep.named, static_cast<unsigned>(after.size()), nameOff,
         rep.matches);
    if (pick < 0) {
        if (rep.method == PickMethod::FileName) {
            logf("gamectl: %s", pick == kPickNoNew ? "the game did not list the copy (no info has its file name)"
                                                   : "several infos have the copy's file name, not guessing");
            // what the names look like (the last three infos of the list), for the next look at a failure
            for (size_t i = after.size() > 3 ? after.size() - 3 : 0; i < after.size(); i++) {
                char nm[300] = {};
                if (infoFileName(after[i], nameOff, nm, sizeof(nm))) logf("gamectl: info #%u file name \"%s\"", static_cast<unsigned>(i), nm);
            }
        }
        else
            logf("gamectl: gold %d (offset %d): %s", hdr.goldMs, goff,
                 pick == kPickNoNew ? "the rescan added no info" : "none or several of the new infos match");
        DeleteFileW(dst);
        return Result::NotFound;
    }
    void* info = after[static_cast<size_t>(pick)];
    int kind = -1, gold = -1;
    readAt(info, static_cast<size_t>(infoKindOffset()), &kind);
    if (goff >= 0) readAt(info, static_cast<size_t>(goff), &gold);
    logf("gamectl: picked info %p (#%d, %s, kind %d, gold %d, file header gold %d)%s", info, pick,
         rep.method == PickMethod::FileName ? "by file name" : "by gold time", kind, gold, hdr.goldMs,
         (hdr.goldMs > 0 && gold != hdr.goldMs) ? " - gold differs from the file header" : "");
    if (!setMenuToPlay(app, menus, info)) return Result::GameError;
    logf("gamectl: menu set to play: SelectedChallengeInfo +0x%X, ChallengeType 3, mode +0x%X = %d, result +0x%X = %d",
         (unsigned)g_spec->menusSelected, (unsigned)g_spec->menusMode, g_spec->modeValue, (unsigned)g_spec->menusResult,
         g_spec->resultValue);
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
