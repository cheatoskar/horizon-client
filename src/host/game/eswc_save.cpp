// save_replay on ESWC (0.1.7.9 unofficial 1.80): the game's "Save replay" without the Save-As dialog.
// Ported from plugins/eswc-autosave (saveReplaySeh), which got it from CGameCtnApp::SaveReplay
// (FUN_0070b2c0): default name "<challenge name>(mm'ss''cc)", FilterFileName, CSystemArchiveNod::Save
// into the folder the replay came from or "My Replays", then ScanDiskForReplays as the game does.
#include "host/game/eswc_save.h"

#include <cstring>
#include <cwchar>

#include "host/host.h"

namespace horizon {
namespace {

// Functions (link addresses, rebased).
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
constexpr uintptr_t kEmptyWideString = 0x00bb45d4; // wchar_t* (text of every empty CFastStringInt)
constexpr uintptr_t kMainEngine = 0x00c96140;      // CMwEngineMain* ; ->+0x28 ->+0x2c = CSystemEngine*

constexpr size_t kAppPrevReplay = 0x25c;     // CGameCtnReplayRecord* previous race (menu "Save previous replay")
constexpr size_t kAppCurReplay = 0x144;      // CGameCtnReplayRecord* current race (solo "Save replay")
constexpr size_t kAppReplaysFolder = 0x210;  // CSystemFids* "My Replays"
constexpr size_t kReplayFid = 0x08;
constexpr size_t kReplayFidBufCount = 0x30;
constexpr size_t kReplayFidBufData = 0x34;
constexpr size_t kFidParentFolder = 0x18;
constexpr size_t kReplayChallenge = 0x60;
constexpr size_t kReplayRecording = 0xb4;
constexpr size_t kReplaySavedFid = 0xdc;
constexpr size_t kChallengeType = 0x118;     // 5 = stunts
constexpr size_t kChallengeName = 0x124;

struct FastStringInt {
    uint32_t len;
    wchar_t* ptr;
};
struct StringParamInt {
    const wchar_t* ptr;
    uint32_t len;
    uint32_t pad;
};

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

template <typename T> T fn(uintptr_t link) { return reinterpret_cast<T>(rebase(link)); }
template <typename T> T field(const void* obj, size_t off) {
    return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(obj) + off);
}

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

} // namespace

// No C++ objects in here (SEH).
void eswcSaveReplay(void* app, bool solo, const wchar_t* stem, EswcSaveResult* r) {
    __try {
        r->ok = -1;
        r->name[0] = r->folder[0] = 0;
        void* replay = field<void*>(app, solo ? kAppCurReplay : kAppPrevReplay);
        if (!replay) { r->why = solo ? "no current replay" : "no previous replay"; return; }
        if (field<int>(replay, kReplayRecording) != 0 && solo) { r->why = "the replay is still recording"; return; }
        if (!solo && field<void*>(replay, kReplaySavedFid)) { r->why = "already saved"; return; }
        void* challenge = field<void*>(replay, kReplayChallenge);
        if (!challenge) { r->why = "the replay has no map"; return; }
        void* folder = field<void*>(app, kAppReplaysFolder);
        if (field<uint32_t>(replay, kReplayFidBufCount) == 1) {
            void** files = field<void**>(replay, kReplayFidBufData);
            void* fidFile = files ? files[0] : nullptr;
            void* parent = fidFile ? field<void*>(fidFile, kFidParentFolder) : nullptr;
            if (parent) folder = parent;
        }
        if (!folder) { r->why = "no replays folder"; return; }

        int time = -1;
        if (field<int>(challenge, kChallengeType) == 5) {
            time = fn<FnBestScore>(kBestStuntScore)(replay, nullptr);
        } else {
            int other = 0;
            fn<FnBestTime>(kBestGhostTime)(replay, nullptr, &time, &other);
        }
        r->time = time;
        wchar_t base[260];
        if (stem && *stem) {
            wcsncpy_s(base, stem, _TRUNCATE);
        } else {
            wchar_t name[200];
            copyOut(field<FastStringInt>(challenge, kChallengeName), name, 200);
            if (field<int>(challenge, kChallengeType) == 5) {
                swprintf_s(base, L"%s(%d)", name, time);
            } else {
                const int t = time < 0 ? -time : time;
                const int mm = t / 60000;
                const unsigned ss = static_cast<unsigned>(t - mm * 60000) / 1000;
                const unsigned cc = static_cast<unsigned>(t - static_cast<int>(ss) * 1000 - mm * 60000) / 10;
                swprintf_s(base, L"%s(%.2d'%.2d''%.2d)", name, mm, ss, cc);
            }
        }

        FastStringInt raw, filtered;
        initString(raw);
        initString(filtered);
        const FnExists exists = fn<FnExists>(kFileExists);
        bool found = false;
        for (int attempt = 1; attempt <= 20 && !found; attempt++) {
            wchar_t full[360];
            if (attempt <= 1) swprintf_s(full, L"%s.Replay.Gbx", base);
            else swprintf_s(full, L"%s (%d).Replay.Gbx", base, attempt);
            setString(raw, full);
            fn<FnFilter>(kFilterFileName)(&filtered, &raw, 0, 0);
            if (!exists(&filtered, folder)) found = true; // never overwrite
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

        FastStringInt path;
        initString(path);
        void** fvt = field<void**>(folder, 0);
        reinterpret_cast<FnGetFullPath>(fvt[0xac / 4])(folder, nullptr, &path);
        copyOut(path, r->folder, 300);
        fn<FnStrEmpty>(kStrEmpty)(&path, nullptr);
        fn<FnStrEmpty>(kStrEmpty)(&raw, nullptr);
        fn<FnStrEmpty>(kStrEmpty)(&filtered, nullptr);
        if (saved) fn<FnScan>(kScanReplays)(app, nullptr); // as the game does after a manual save
        r->ok = saved ? 1 : 0;
        r->why = saved ? "" : "CSystemArchiveNod save returned 0";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r->ok = 0;
        r->why = "exception inside the game's save code";
    }
}

} // namespace horizon
