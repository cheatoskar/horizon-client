// ESWC Autosave 2.0 - a Horizon Host plugin for TrackMania Nations ESWC (unofficial 1.80, exe 0.1.7.9).
//
// What the game does when the player picks "Save previous replay" in the in-game menu
// (CGameCtnApp::SaveReplay, FUN_0070b2c0): it builds the default name "<challenge name>(mm'ss''cc)",
// shows the Save-As dialog, appends ".Replay.Gbx", filters the name and writes the file with
// CSystemArchiveNod (FUN_0062e1c0) into the "My Replays" folder (app+0x210). We run exactly that
// post-dialog part, without the dialogs.
//
// Version 1 hooked the game itself. Version 2 gets the same moments as host events:
//   FINISH    - the local player finished (right after CTrackManiaRace::OnFinishLine). We remember it.
//   RACE_END  - the game stopped recording and holds the replay (engine_replay). If a finish is pending,
//               we save that replay: end_screen 1 = solo (current replay app+0x144, kept by the game),
//               end_screen 0 = LAN/online (the previous-race copy app+0x25c, "already saved" marker).
//   RACE_START / MAP_LOADED clear a pending finish.
// Everything runs on the game's main thread, inside the game's own calls. No hooks here.
//
// TMX copy: after a LAN/online save, a worker thread re-reads the file and, if it holds more than one
// ghost, writes "<name> TMX.Replay.Gbx" with only the local player's ghost (tmxcopy.cpp). The worker
// never touches the host: its result is queued to the game thread (run_on_game_thread).
#define HORIZON_PLUGIN_BUILD
#include <windows.h>
#include <errno.h>
#include <stdarg.h>
#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "horizon_imgui.h"
#include "horizon_plugin.h"
#include "tmxcopy.h"

namespace {

constexpr char kVersion[] = "2.0.0";
constexpr char kId[] = "eswc-autosave";
constexpr char kExeSha256[] = "983bd12724739dcb0f876aeb3a4259154b068940f5167f57ede750ca452d8068";
constexpr uintptr_t kPreferredBase = 0x00400000;

// Functions (link addresses, rebased with the exe's base).
constexpr uintptr_t kStrSet = 0x00405f40;         // void CFastStringInt::SetString(const SStringParamInt&)  ret 4
constexpr uintptr_t kStrEmpty = 0x00402530;       // void CFastStringInt::Empty()
constexpr uintptr_t kFilterFileName = 0x00635870; // cdecl (CFastStringInt& dst, const CFastStringInt& src, int, int)
constexpr uintptr_t kFileExists = 0x0063f260;     // cdecl int (const CFastStringInt& name, CSystemFids* folder)
constexpr uintptr_t kBestGhostTime = 0x0068c390;  // void CGameCtnReplayRecord::GetBestGhostRaceTime(int&, int&)  ret 8
constexpr uintptr_t kBestStuntScore = 0x0068c470; // int CGameCtnReplayRecord::GetBestGhostStuntsScore()
constexpr uintptr_t kArchiveCtor = 0x0062a850;    // CSystemArchiveNod::CSystemArchiveNod()
constexpr uintptr_t kArchiveSave = 0x0062e1c0;    // int CSystemArchiveNod::Save(name&, nod, folder, 10, 7, 1, 1)  ret 0x1c
constexpr uintptr_t kArchiveDtor = 0x0062a950;    // CSystemArchiveNod::~CSystemArchiveNod()
constexpr uintptr_t kUnbindFid = 0x00623f90;      // void CSystemEngine::UnbindFid(CMwNod*)  ret 4
constexpr uintptr_t kScanReplays = 0x007173b0;    // void CTrackMania::ScanDiskForReplays()
// Globals
constexpr uintptr_t kEmptyWideString = 0x00bb45d4; // wchar_t* (text of every empty CFastStringInt)
constexpr uintptr_t kMainEngine = 0x00c96140;      // CMwEngineMain* ; ->+0x28 ->+0x2c = CSystemEngine*

// Offsets
constexpr size_t kAppPrevReplay = 0x25c;     // CGameCtnReplayRecord* previous race (menu "Save previous replay")
constexpr size_t kAppCurReplay = 0x144;      // CGameCtnReplayRecord* current race (solo "Save replay")
constexpr size_t kAppReplaysFolder = 0x210;  // CSystemFids* "My Replays"
constexpr size_t kReplayFid = 0x08;          // CSystemFidFile* while bound
constexpr size_t kReplayFidBufCount = 0x30;  // count of a fid buffer (1 = replay came from a file)
constexpr size_t kReplayFidBufData = 0x34;   // CSystemFidFile** of that buffer
constexpr size_t kFidParentFolder = 0x18;    // CSystemFidFile -> CSystemFids* (folder)
constexpr size_t kReplayChallenge = 0x60;    // CGameCtnChallenge*
constexpr size_t kReplayRecording = 0xb4;    // != 0 while recording
constexpr size_t kReplaySavedFid = 0xdc;     // fid of the last save (0 = never saved)
constexpr size_t kChallengeType = 0x118;     // 5 = stunts
constexpr size_t kChallengeName = 0x124;     // CFastStringInt {len, ptr}

struct FastStringInt {
    uint32_t len;
    wchar_t* ptr;
};
struct StringParamInt {
    const wchar_t* ptr;
    uint32_t len;
    uint32_t pad; // unused by this build, zero
};

// thiscall functions are called as fastcall with a dummy edx.
using FnStrSet = void(__fastcall*)(FastStringInt* self, void* edx, const StringParamInt* src);
using FnStrEmpty = void(__fastcall*)(FastStringInt* self, void* edx);
using FnFilter = void(__cdecl*)(FastStringInt* dst, const FastStringInt* src, int, int);
using FnExists = int(__cdecl*)(const FastStringInt* name, void* folder);
using FnBestTime = void(__fastcall*)(void* replay, void* edx, int* time, int* other);
using FnBestScore = int(__fastcall*)(void* replay, void* edx);
using FnArchCtor = void(__fastcall*)(void* archive, void* edx);
using FnArchSave = int(__fastcall*)(void* archive, void* edx, FastStringInt* name, void* nod, void* folder, int version,
                                    int flags, int a, int b);
using FnArchDtor = void(__fastcall*)(void* archive, void* edx);
using FnUnbind = void(__fastcall*)(void* systemEngine, void* edx, void* nod);
using FnScan = void(__fastcall*)(void* app, void* edx);
using FnVoidMethod = void(__fastcall*)(void* self, void* edx);
using FnGetFullPath = void(__fastcall*)(void* fids, void* edx, FastStringInt* out);

uintptr_t g_base = kPreferredBase;
uintptr_t rebase(uintptr_t link) { return g_base - kPreferredBase + link; }
template <typename T> T fn(uintptr_t link) { return reinterpret_cast<T>(rebase(link)); }
template <typename T> T field(const void* obj, size_t off) {
    return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(obj) + off);
}

// ---------------------------------------------------------------- plugin state --------------
const HorizonHostApi* g_host = nullptr;
bool g_imgui = false;
bool g_enabled = true;
bool g_passive = false; // the standalone ESWC Autosave is active in this process: this module does nothing

#define LOGI(...) g_host->log(g_host->plugin, HORIZON_LOG_INFO, __VA_ARGS__)
#define LOGW(...) g_host->log(g_host->plugin, HORIZON_LOG_WARN, __VA_ARGS__)
#define LOGE(...) g_host->log(g_host->plugin, HORIZON_LOG_ERROR, __VA_ARGS__)

struct Config {
    bool bestOnly = true;
    bool scanReplays = true;
    bool tmxCopy = true;
} g_cfg;

int32_t g_hotkey = 0;
bool g_pending = false; // a local finish is waiting for its replay
int g_pendingTime = -1;
bool g_saving = false;
volatile LONG g_tmxRunning = 0;

int g_savedCount = 0;
char g_lastResult[400] = "";

void setLast(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(g_lastResult, sizeof(g_lastResult), _TRUNCATE, fmt, ap);
    va_end(ap);
}

void toUtf8(const wchar_t* w, char* out, int size) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, size, nullptr, nullptr)) out[0] = 0;
}

// ---------------------------------------------------------------- standalone Autosave ------
// The standalone eswc-autosave (dinput8.dll + eswc_autosave.dll next to the exe, or chained by the Legacy Loader
// as dinput8.chain.dll) hooks the game itself. Two of them would save every replay twice, so this module
// stays passive when the standalone one is there. Its files are only looked at, never changed.
constexpr char kStandaloneNote[] = "Eigenst\xC3\xA4ndiges ESWC Autosave gefunden \xE2\x80\x93 Modul pausiert";

bool standaloneFound(const HorizonGameInfo* gi) {
    if (GetModuleHandleW(L"eswc_autosave.dll")) return true; // its loader already started it
    wchar_t dir[MAX_PATH] = L"";
    if (gi && gi->game_folder && *gi->game_folder) {
        MultiByteToWideChar(CP_UTF8, 0, gi->game_folder, -1, dir, MAX_PATH);
    } else {
        DWORD n = GetModuleFileNameW(nullptr, dir, MAX_PATH);
        wchar_t* slash = (n && n < MAX_PATH) ? wcsrchr(dir, L'\\') : nullptr;
        if (slash) slash[1] = 0;
        else dir[0] = 0;
    }
    const size_t len = wcslen(dir);
    if (!len) return false;
    const wchar_t* sep = (dir[len - 1] == L'\\' || dir[len - 1] == L'/') ? L"" : L"\\";
    wchar_t path[MAX_PATH + 32];
    swprintf_s(path, L"%s%seswc_autosave.dll", dir, sep);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;
    swprintf_s(path, L"%s%sdinput8.chain.dll", dir, sep); // standalone files there and chained = it runs
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

void HORIZON_CALL standaloneToast(void*) {
    if (!g_host || g_host->abi_minor < 3 || !g_host->notify) return;
    HorizonToast t = {};
    t.size = sizeof(t);
    t.kind = HORIZON_TOAST_WARN;
    t.title = kStandaloneNote;
    t.text = "";
    t.duration_ms = 8000;
    g_host->notify(g_host->plugin, &t);
}

// ---------------------------------------------------------------- config --------------------
void saveConfig() {
    g_host->set_string(g_host->plugin, "Mode", g_cfg.bestOnly ? "best" : "all");
    g_host->set_int(g_host->plugin, "ScanReplays", g_cfg.scanReplays ? 1 : 0);
    g_host->set_int(g_host->plugin, "TmxCopy", g_cfg.tmxCopy ? 1 : 0);
}

// One time: take over the values of an old eswc_autosave.ini next to the exe.
void importIni() {
    if (g_host->get_int(g_host->plugin, "IniImported", 0)) return;
    g_host->set_int(g_host->plugin, "IniImported", 1);
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = (n && n < MAX_PATH) ? wcsrchr(path, L'\\') : nullptr;
    if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"eswc_autosave.ini");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return;
    wchar_t mode[32] = L"";
    GetPrivateProfileStringW(L"autosave", L"Mode", L"best", mode, 32, path);
    const int scan = GetPrivateProfileIntW(L"autosave", L"ScanReplays", 1, path) != 0;
    const int tmx = GetPrivateProfileIntW(L"autosave", L"TmxCopy", 1, path) != 0;
    g_host->set_string(g_host->plugin, "Mode", _wcsicmp(mode, L"all") == 0 ? "all" : "best");
    g_host->set_int(g_host->plugin, "ScanReplays", scan);
    g_host->set_int(g_host->plugin, "TmxCopy", tmx);
    LOGI("config: imported eswc_autosave.ini once (Mode=%s ScanReplays=%d TmxCopy=%d)", _wcsicmp(mode, L"all") == 0 ? "all" : "best", scan, tmx);
}

void loadConfig() {
    char mode[16] = "";
    g_host->get_string(g_host->plugin, "Mode", "best", mode, sizeof(mode));
    g_cfg.bestOnly = _stricmp(mode, "all") != 0;
    g_cfg.scanReplays = g_host->get_int(g_host->plugin, "ScanReplays", 1) != 0;
    g_cfg.tmxCopy = g_host->get_int(g_host->plugin, "TmxCopy", 1) != 0;
    LOGI("config: Mode=%s ScanReplays=%d TmxCopy=%d", g_cfg.bestOnly ? "best" : "all", g_cfg.scanReplays, g_cfg.tmxCopy);
}

// ---------------------------------------------------------------- saving --------------------
struct SaveResult {
    int ok;           // 1 saved, 0 failed, -1 skipped
    const char* why;  // for skipped/failed
    wchar_t name[300];
    wchar_t folder[300];
    int time;
    bool fromFile;
};

void initString(FastStringInt& s) {
    s.len = 0;
    s.ptr = *reinterpret_cast<wchar_t**>(rebase(kEmptyWideString));
}

void setString(FastStringInt& s, const wchar_t* text) {
    StringParamInt p = {text, static_cast<uint32_t>(wcslen(text)), 0};
    fn<FnStrSet>(kStrSet)(&s, nullptr, &p);
}

void copyOut(const FastStringInt& s, wchar_t* out, size_t cap) {
    size_t n = s.len < cap - 1 ? s.len : cap - 1;
    if (s.ptr && n) memcpy(out, s.ptr, n * sizeof(wchar_t));
    out[n] = 0;
}

// The part of FUN_0070b2c0 after the Save-As dialog, with the dialog's default answer.
// No C++ objects in here (SEH).
// solo: the game's current replay (app+0x144), which the game keeps using; we leave its
// "saved as" field (+0xdc) alone there so the game's own "Save replay" behaves as before.
void saveReplaySeh(void* app, void* replay, bool solo, bool scanReplays, SaveResult* r) {
    __try {
        r->ok = -1;
        if (!replay) { r->why = solo ? "no current replay" : "no previous replay"; return; }
        if (field<int>(replay, kReplayRecording) != 0 && solo) { r->why = "replay is still recording"; return; }
        if (!solo && field<void*>(replay, kReplaySavedFid)) { r->why = "already saved"; return; }
        void* challenge = field<void*>(replay, kReplayChallenge);
        if (!challenge) { r->why = "replay has no challenge"; return; }
        // Folder as SaveReplay picks it: the folder of the file the replay came from, else "My Replays".
        void* folder = field<void*>(app, kAppReplaysFolder);
        r->fromFile = false;
        if (field<uint32_t>(replay, kReplayFidBufCount) == 1) {
            void** files = field<void**>(replay, kReplayFidBufData);
            void* fidFile = files ? files[0] : nullptr;
            void* parent = fidFile ? field<void*>(fidFile, kFidParentFolder) : nullptr;
            if (parent) {
                folder = parent;
                r->fromFile = true;
            }
        }
        if (!folder) { r->why = "no replays folder"; return; }

        // Default name exactly as the game builds it: challenge name + "(mm'ss''cc)" (stunts: "(score)").
        wchar_t base[300];
        const FastStringInt name = field<FastStringInt>(challenge, kChallengeName);
        copyOut(name, base, 200);
        wchar_t suffix[40];
        if (field<int>(challenge, kChallengeType) == 5) {
            const int score = fn<FnBestScore>(kBestStuntScore)(replay, nullptr);
            swprintf_s(suffix, L"(%d)", score);
            r->time = score;
        } else {
            int time = -1, other = 0;
            fn<FnBestTime>(kBestGhostTime)(replay, nullptr, &time, &other);
            r->time = time;
            int t = time < 0 ? -time : time;
            const int mm = t / 60000;
            const unsigned ss = static_cast<unsigned>(t - mm * 60000) / 1000;
            const unsigned cc = static_cast<unsigned>(t - static_cast<int>(ss) * 1000 - mm * 60000) / 10;
            swprintf_s(suffix, L"(%.2d'%.2d''%.2d)", mm, ss, cc);
        }

        FastStringInt raw, filtered;
        initString(raw);
        initString(filtered);
        const FnExists exists = fn<FnExists>(kFileExists);
        bool found = false;
        wchar_t stem[260];
        swprintf_s(stem, L"%s%s", base, suffix);
        for (int attempt = 1; attempt <= 20 && !found; attempt++) {
            wchar_t full[360];
            // "<stem>.Replay.Gbx" / "<stem> (n).Replay.Gbx"
            if (attempt <= 1) swprintf_s(full, L"%s.Replay.Gbx", stem);
            else swprintf_s(full, L"%s (%d).Replay.Gbx", stem, attempt);
            setString(raw, full);
            fn<FnFilter>(kFilterFileName)(&filtered, &raw, 0, 0);
            if (!exists(&filtered, folder)) found = true; // the game would ask "Overwrite?"; we never overwrite
        }
        copyOut(filtered, r->name, 300);
        if (!found) {
            fn<FnStrEmpty>(kStrEmpty)(&raw, nullptr);
            fn<FnStrEmpty>(kStrEmpty)(&filtered, nullptr);
            r->why = "file exists (20 names tried)";
            return;
        }

        if (field<int>(replay, kReplayRecording) != 0) {
            void** vt = field<void**>(replay, 0);
            reinterpret_cast<FnVoidMethod>(vt[0xa0 / 4])(replay, nullptr);
        }

        alignas(16) static unsigned char archive[0x400];
        memset(archive, 0, sizeof(archive));
        fn<FnArchCtor>(kArchiveCtor)(archive, nullptr);
        const int saved = fn<FnArchSave>(kArchiveSave)(archive, nullptr, &filtered, replay, folder, 10, 7, 1, 1);
        fn<FnArchDtor>(kArchiveDtor)(archive, nullptr);

        if (saved && !solo) {
            *reinterpret_cast<void**>(static_cast<uint8_t*>(replay) + kReplaySavedFid) = field<void*>(replay, kReplayFid);
        }
        void* engineMain = *reinterpret_cast<void**>(rebase(kMainEngine));
        void* systemEngine = field<void*>(field<void*>(engineMain, 0x28), 0x2c);
        fn<FnUnbind>(kUnbindFid)(systemEngine, nullptr, replay);

        // Folder path, for the log only.
        FastStringInt path;
        initString(path);
        void** fvt = field<void**>(folder, 0);
        reinterpret_cast<FnGetFullPath>(fvt[0xac / 4])(folder, nullptr, &path);
        copyOut(path, r->folder, 300);
        fn<FnStrEmpty>(kStrEmpty)(&path, nullptr);
        fn<FnStrEmpty>(kStrEmpty)(&raw, nullptr);
        fn<FnStrEmpty>(kStrEmpty)(&filtered, nullptr);

        if (saved && scanReplays) fn<FnScan>(kScanReplays)(app, nullptr); // as the game does after a manual save
        r->ok = saved ? 1 : 0;
        r->why = saved ? "" : "CSystemArchiveNod save returned 0";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r->ok = 0;
        r->why = "exception inside the game's save code";
    }
}

// Reads the header the game wrote (the XML chunk) and reports what matters for validation.
void checkHeader(const wchar_t* path, char* out, size_t cap) {
    out[0] = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        _snprintf_s(out, cap, _TRUNCATE, "header: file not readable (%lu)", GetLastError());
        return;
    }
    static char buf[16384];
    DWORD got = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(f);
    buf[got] = 0;
    for (DWORD i = 0; i < got; i++)
        if (!buf[i]) buf[i] = ' ';
    const bool gbx = got > 3 && memcmp(buf, "GBX", 3) == 0;
    const char* xml = strstr(buf, "<header type=\"replay\"");
    const char* val = strstr(buf, "validable=\"");
    const char* best = strstr(buf, "best=\"");
    const char* exever = strstr(buf, "exever=\"");
    const char v = val ? val[11] : '?';
    const int bestMs = best ? atoi(best + 6) : -1;
    char ver[16] = "?";
    if (exever) {
        size_t k = 0;
        for (const char* p = exever + 8; *p && *p != '"' && k < sizeof(ver) - 1; p++) ver[k++] = *p;
        ver[k] = 0;
    }
    _snprintf_s(out, cap, _TRUNCATE, "header: %s, xml %s, exever=%s, best=%d, validable=%c%s", gbx ? "GBX" : "NOT GBX",
                xml ? "ok" : "MISSING", ver, bestMs, v, (gbx && xml && v == '1') ? "" : "  <-- WARNING");
}

// ---------------------------------------------------------------- TMX copy ------------------
// Runs in its own thread after the game's save has returned, so the game never waits for it.
// It only reads the saved file and writes a new one next to it; no game memory is touched and
// the host API is not called from here (the result goes through run_on_game_thread).
struct TmxJob {
    wchar_t path[620];
    int localTime; // local player's finish time that triggered the save, -1 = unknown (hotkey)
};

struct TmxDone {
    int status; // autosave::tmxcopy::Status
    char text[400];
    char outPath[1100]; // UTF-8, only when status == Converted
};

void HORIZON_CALL tmxDoneOnGameThread(void* user) {
    TmxDone* d = static_cast<TmxDone*>(user);
    if (d->status == static_cast<int>(autosave::tmxcopy::Status::Converted)) {
        LOGI("[TMX] SAVED: %s", d->text);
        g_host->post_replay_saved(g_host->plugin, d->outPath);
    } else {
        LOGI("[TMX] %s", d->text);
    }
    free(d);
}

unsigned __stdcall tmxThread(void* arg) {
    TmxJob* job = static_cast<TmxJob*>(arg);
    autosave::tmxcopy::Report r;
    autosave::tmxcopy::makeCopy(job->path, nullptr, nullptr, job->localTime, &r);
    TmxDone* d = static_cast<TmxDone*>(calloc(1, sizeof(TmxDone)));
    if (d) {
        d->status = static_cast<int>(r.status);
        switch (r.status) {
        case autosave::tmxcopy::Status::Converted: {
            char p[1100];
            toUtf8(r.outPath, p, sizeof(p));
            strcpy_s(d->outPath, p);
            _snprintf_s(d->text, sizeof(d->text), _TRUNCATE,
                        "%s ghosts %u->1 time=%d validable=%d (kept ghost #%d, took %.1f ms off the game thread)", p,
                        r.ghostsBefore, r.time, r.validable, r.keptIndex, r.ms);
            break;
        }
        case autosave::tmxcopy::Status::NotNeeded: _snprintf_s(d->text, sizeof(d->text), _TRUNCATE, "not needed: %s", r.reason); break;
        case autosave::tmxcopy::Status::Refused: _snprintf_s(d->text, sizeof(d->text), _TRUNCATE, "skipped: %s", r.reason); break;
        default: _snprintf_s(d->text, sizeof(d->text), _TRUNCATE, "ERROR: %s", r.reason); break;
        }
        g_host->run_on_game_thread(g_host->plugin, tmxDoneOnGameThread, d);
    }
    free(job);
    InterlockedDecrement(&g_tmxRunning);
    return 0;
}

void startTmxCopy(const wchar_t* path, int localTime) {
    TmxJob* job = static_cast<TmxJob*>(malloc(sizeof(TmxJob)));
    if (!job) return;
    if (wcslen(path) >= _countof(job->path)) {
        LOGI("[TMX] skipped: path too long");
        free(job);
        return;
    }
    wcscpy_s(job->path, path);
    job->localTime = localTime;
    InterlockedIncrement(&g_tmxRunning);
    const uintptr_t h = _beginthreadex(nullptr, 256 * 1024, tmxThread, job, 0, nullptr);
    if (!h) {
        LOGE("[TMX] ERROR: could not start the worker thread (errno %d)", errno);
        InterlockedDecrement(&g_tmxRunning);
        free(job);
        return;
    }
    SetThreadPriority(reinterpret_cast<HANDLE>(h), THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(reinterpret_cast<HANDLE>(h));
}

// localTime: the local player's finish time that led to this save (-1 = unknown); the TMX copy
// keeps only a ghost with exactly this time.
void saveNow(void* replay, bool solo, const char* reason, int localTime) {
    const char* mode = solo ? "Solo" : "LAN";
    if (g_saving) return;
    void* app = g_host->engine_pointer(HORIZON_ENGINE_APP);
    if (!app) {
        LOGI("[%s] save (%s): game app not available, nothing to save", mode, reason);
        setLast("Skipped: game not ready");
        return;
    }
    g_saving = true;
    SaveResult r = {};
    const DWORD t0 = GetTickCount();
    saveReplaySeh(app, replay, solo, g_cfg.scanReplays, &r);
    const DWORD ms = GetTickCount() - t0;
    g_saving = false;
    char name[600], folder[600];
    toUtf8(r.name, name, sizeof(name));
    toUtf8(r.folder, folder, sizeof(folder));
    if (r.ok == 1) {
        wchar_t full[620];
        swprintf_s(full, L"%s%s", r.folder, r.name);
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        const BOOL have = GetFileAttributesExW(full, GetFileExInfoStandard, &fa);
        LOGI("[%s] SAVED (%s): %s%s  time=%d  size=%lu bytes  took %lu ms%s", mode, reason, folder, name, r.time,
             have ? fa.nFileSizeLow : 0ul, ms, r.fromFile ? "  (folder of the replay's source file, as the game does)" : "");
        char hdr[200];
        checkHeader(full, hdr, sizeof(hdr));
        LOGI("[%s] %s", mode, hdr);
        g_savedCount++;
        if (solo || r.time < 0) setLast("Saved: %s (%d)", name, r.time);
        else setLast("Saved: %s (%d.%02d)", name, r.time / 1000, (r.time % 1000) / 10);
        char fullUtf8[1300];
        toUtf8(full, fullUtf8, sizeof(fullUtf8));
        g_host->post_replay_saved(g_host->plugin, fullUtf8);
        if (!solo && g_cfg.tmxCopy) startTmxCopy(full, localTime); // network race: maybe several ghosts
    } else if (r.ok == 0) {
        LOGE("[%s] ERROR (%s): %s  [%s]", mode, reason, r.why, name);
        setLast("Failed: %s", r.why);
    } else {
        LOGI("[%s] skipped (%s): %s", mode, reason, r.why);
        setLast("Skipped: %s", r.why);
    }
}

// Same choice the old test key made: the previous-race copy, else the current (solo) replay.
void saveNowManual() {
    void* app = g_host->engine_pointer(HORIZON_ENGINE_APP);
    void* replay = nullptr;
    bool solo = false;
    if (app) {
        __try {
            replay = field<void*>(app, kAppPrevReplay);
            if (!replay) {
                replay = field<void*>(app, kAppCurReplay);
                solo = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            replay = nullptr;
        }
    }
    saveNow(replay, solo, "hotkey", -1);
}

void HORIZON_CALL onHotkey(void*) {
    if (g_enabled && !g_passive) saveNowManual();
}

}  // namespace

// ================================================================ exports =====================
extern "C" {

static const HorizonPluginInfo kInfo = {
    HORIZON_ABI,
    kId,
    "Autosave & Login-Fix",
    kVersion,
    HORIZON_GAME_ESWC,
    HORIZON_CAP_MENU | HORIZON_CAP_GAME_EVENTS,
    HORIZON_FLAG_ONLINE_OK | HORIZON_FLAG_LIVE_TOGGLE,
    "cheatoskar",
    "Saves your replay after each finish. The ESWC login fix (error 12157) is part of Horizon.",
    HORIZON_ABI_MINOR,
};

HORIZON_EXPORT const HorizonPluginInfo* HORIZON_CALL HorizonPlugin_Info(void) { return &kInfo; }

HORIZON_EXPORT int32_t HORIZON_CALL HorizonPlugin_Init(const HorizonHostApi* host) {
    if (!host || host->abi != HORIZON_ABI) return 1;
    g_host = host;
    const HorizonGameInfo* gi = host->game_info ? host->game_info() : nullptr;
    if (!gi || gi->game != HORIZON_GAME_ESWC || !gi->exe_sha256 || _stricmp(gi->exe_sha256, kExeSha256) != 0) {
        LOGW("refusing to load: not TmNationsESWC 1.80 (sha256 %s)", gi && gi->exe_sha256 ? gi->exe_sha256 : "?");
        return 2;
    }
    g_base = gi->exe_base ? gi->exe_base : kPreferredBase;
    LOGI("---- eswc-autosave %s start, TmNationsESWC 1.80 recognised, base %08X ----", kVersion, (unsigned)g_base);
    g_passive = standaloneFound(gi);
    if (g_passive) {
        LOGW("standalone ESWC Autosave found (eswc_autosave.dll): module paused, nothing hooked or subscribed");
        g_imgui = horizon_imgui_attach(host);
        host->run_on_game_thread(host->plugin, standaloneToast, nullptr);
        return 0;
    }
    g_imgui = horizon_imgui_attach(host);
    if (!g_imgui) LOGW("ImGui attach failed, working without the menu page");
    importIni();
    loadConfig();
    g_hotkey = host->register_hotkey(host->plugin, "save", "Save replay now", "F9", onHotkey, nullptr);
    if (!g_hotkey) LOGW("could not register the save hotkey");
    host->subscribe(host->plugin, HORIZON_EVENT_BIT(HORIZON_EVENT_FINISH) | HORIZON_EVENT_BIT(HORIZON_EVENT_RACE_END) |
                                      HORIZON_EVENT_BIT(HORIZON_EVENT_RACE_START) | HORIZON_EVENT_BIT(HORIZON_EVENT_MAP_LOADED));
    LOGI("ready: saving after every %s of the local player", g_cfg.bestOnly ? "new best finish" : "finish");
    return 0;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_Shutdown(void) {
    // Let a running TMX worker finish (it only reads/writes files), at most 5 s.
    for (int i = 0; i < 500 && g_tmxRunning > 0; i++) Sleep(10);
    if (g_host) g_host->log(g_host->plugin, HORIZON_LOG_INFO, "shutdown, %d replays saved this session", g_savedCount);
    g_host = nullptr;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_SetEnabled(int32_t enabled) {
    g_enabled = enabled != 0;
    if (!g_enabled) g_pending = false;
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_OnEvent(const HorizonEvent* e) {
    if (!e || !g_enabled || g_passive) return;
    switch (e->type) {
    case HORIZON_EVENT_MAP_LOADED:
    case HORIZON_EVENT_RACE_START:
        g_pending = false;
        break;
    case HORIZON_EVENT_FINISH: {
        if (!e->is_local_player) break;
        const bool isPb = e->is_pb != 0;
        LOGI("finish: local player, time=%d%s", e->time_ms, isPb ? "  NEW BEST" : "");
        if (g_cfg.bestOnly && !isPb) {
            LOGI("finish: Mode=best and no new best, not saving");
            g_pending = false;
            break;
        }
        g_pending = true;
        g_pendingTime = e->time_ms;
        break;
    }
    case HORIZON_EVENT_RACE_END: {
        if (!g_pending) break; // not after a finish of ours
        g_pending = false;
        const bool solo = e->end_screen != 0;
        LOGI("[%s] race end after the finish (time=%d), saving %s", solo ? "Solo" : "LAN", g_pendingTime,
             solo ? "the race replay" : "the previous replay");
        saveNow(e->engine_replay, solo, "finish", g_pendingTime);
        break;
    }
    default: break;
    }
}

HORIZON_EXPORT void HORIZON_CALL HorizonPlugin_DrawMenu(void) {
    if (!g_imgui || !g_host) return;
    if (g_passive) {
        ImGui::TextWrapped("%s", kStandaloneNote);
        ImGui::TextDisabled("Remove dinput8.chain.dll to use this module instead.");
        return;
    }
    bool changed = false;
    ImGui::TextUnformatted("Save");
    int mode = g_cfg.bestOnly ? 0 : 1;
    if (ImGui::RadioButton("New best only", &mode, 0)) changed = true;
    ImGui::SameLine();
    if (ImGui::RadioButton("Every finish", &mode, 1)) changed = true;
    g_cfg.bestOnly = mode == 0;
    if (ImGui::Checkbox("Refresh replay list", &g_cfg.scanReplays)) changed = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rescan the game's replay list after each save.");
    if (ImGui::Checkbox("TMX copy", &g_cfg.tmxCopy)) changed = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("After a LAN/online save, also write a copy with only your ghost.");
    if (changed) saveConfig();

    ImGui::Separator();
    const char* key = g_hotkey ? g_host->hotkey_text(g_host->plugin, g_hotkey) : "";
    if (ImGui::Button("Save now")) saveNowManual();
    ImGui::SameLine();
    ImGui::Text("Key: %s", (key && *key) ? key : "none");

    ImGui::Separator();
    ImGui::TextWrapped("%s", g_lastResult[0] ? g_lastResult : "Nothing saved yet.");
    ImGui::Text("Saved this session: %d", g_savedCount);
}

}  // extern "C"
