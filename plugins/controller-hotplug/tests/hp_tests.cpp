// Unit tests of controller-hotplug (Controller fix): the tracker against a game in memory (add, remove, re-add same
// GUID keeps the slot, no registration storm, replug refresh once), the worker's enumeration logic (list series after
// a device change, device descriptions, diff, why the game cannot see a pad) and the game backend against a fake
// input port laid out like ESWC's (enumeration callback appends a pad with DirectInput's device type, binding tables
// get an empty range, refresh swaps the device) and the bindings a late pad gets (the game's defaults pass for
// the current profile, saved bindings kept, appended to the running race's table and the menu tables).
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>

#include <dinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "../hp_enum.h"
#include "../hp_game.h"
#include "../hp_tracker.h"

using namespace hp;

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const char* what) {
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL  %s\n", what);
    }
}

static Guid G(unsigned id) {
    Guid g;
    memset(g.b, 0, 16);
    memcpy(g.b, &id, 4);
    g.b[15] = 0x55;
    return g;
}
static Attached A(unsigned id, const wchar_t* name) { return Attached{G(id), G(1000 + id), name}; }
static int countNote(const std::vector<Event>& ev, Note n) {
    int c = 0;
    for (const auto& e : ev)
        if (e.what == n) c++;
    return c;
}

// ------------------------------------------------------------------ tracker
static void trackerTests() {
    printf("tracker\n");
    {
        FakeGame game;
        Tracker t;
        std::vector<Event> ev;
        check(!t.step(game, {}, 0, 0, ev) && ev.empty() && game.adds == 0, "no list yet: nothing");
        game.readyFlag = false;
        check(t.step(game, {A(1, L"Pad")}, 1, 0, ev) && game.adds == 0, "port not ready: wants another step, adds nothing");
    }
    {
        // a pad present at start is the game's own; nothing happens
        FakeGame game;
        game.pads.push_back(Slot{2, G(1), true});
        Tracker t;
        std::vector<Event> ev;
        t.step(game, {A(1, L"Start pad")}, 1, 0, ev);
        check(ev.empty() && game.adds == 0, "pad present at start: no add, no note");
        t.step(game, {A(1, L"Start pad")}, 1, 5000, ev);
        check(ev.empty() && game.refreshes == 0, "first list never schedules a refresh of never-polled pads");

        // plug in B
        std::vector<Attached> both = {A(1, L"Start pad"), A(2, L"Pad B")};
        t.step(game, both, 2, 6000, ev);
        check(game.adds == 1 && countNote(ev, Note::Added) == 1 && ev.back().name == L"Pad B", "new pad: registered once, note Added");
        check(game.pads.size() == 2 && game.pads[1].index == 3, "new pad got slot 3");
        ev.clear();
        for (int i = 0; i < 20; i++) t.step(game, both, 2, 6100 + i * 100, ev);
        check(game.adds == 1 && ev.empty(), "same list again and again: no second registration (no storm)");

        // unplug B
        t.step(game, {A(1, L"Start pad")}, 3, 9000, ev);
        check(countNote(ev, Note::Gone) == 1 && game.pads.size() == 2, "unplugged: note Gone, the game keeps the slot");
        ev.clear();

        // replug B: same slot, no new registration; the game gets it back by itself
        game.pads[1].lost = false;
        t.step(game, both, 4, 10000, ev);
        check(countNote(ev, Note::Back) == 1 && ev[0].slot == 3 && game.adds == 1, "replug: note Back, same slot 3, no add");
        t.step(game, both, 4, 13000, ev);
        check(game.refreshes == 0, "replug, game reacquired by itself: no refresh");
        ev.clear();

        // unplug and replug again, this time the game cannot reacquire it
        t.step(game, {A(1, L"Start pad")}, 5, 14000, ev);
        game.pads[1].lost = true;
        t.step(game, both, 6, 15000, ev);
        bool more = t.step(game, both, 6, 16000, ev);
        check(game.refreshes == 0 && more, "still lost 1 s after the replug: waits");
        t.step(game, both, 6, 17100, ev);
        check(game.refreshes == 1 && countNote(ev, Note::Refreshed) == 1, "still lost 2 s after the replug: one refresh");
        game.pads[1].lost = true; // even if it stays lost
        for (int i = 0; i < 10; i++) t.step(game, both, 6, 18000 + i * 1000, ev);
        check(game.refreshes == 1, "refresh only once per replug");

        // pulled and plugged in within a moment: same set, new list after a notification
        ev.clear();
        game.pads[0].lost = false; // the start pad is in use
        t.step(game, both, 7, 30000, ev);
        t.step(game, both, 7, 32100, ev);
        check(game.refreshes == 2 && countNote(ev, Note::Back) == 0, "quick replug (same set, new list): one check, one refresh");
        check(game.adds == 1, "after all that still one registration");
    }
    {
        // failing registrations: 30 s apart, 3 at most, start over after a replug
        FakeGame game;
        game.failAdds = true;
        Tracker t;
        std::vector<Event> ev;
        std::vector<Attached> l = {A(7, L"Bad pad")};
        t.step(game, l, 1, 0, ev);
        for (int i = 1; i < 290; i++) t.step(game, l, 1, i * 100, ev);
        check(game.adds == 1 && countNote(ev, Note::AddFailed) == 1, "failed add: no retry within 30 s");
        t.step(game, l, 1, 30000, ev);
        check(game.adds == 2, "retry after 30 s");
        t.step(game, l, 1, 60000, ev);
        check(game.adds == 3 && countNote(ev, Note::AddGaveUp) == 1, "third failure: gives up");
        for (int i = 0; i < 10; i++) t.step(game, l, 1, 100000 + i * 40000, ev);
        check(game.adds == 3, "gave up: no more attempts while plugged in");
        t.step(game, {}, 2, 600000, ev);
        game.failAdds = false;
        t.step(game, l, 3, 601000, ev);
        check(game.adds == 4 && countNote(ev, Note::Added) == 1, "plugged in again: starts over and works");
    }
    {
        FakeGame game;
        Tracker t;
        std::vector<Event> ev;
        std::vector<Attached> l;
        for (unsigned i = 0; i < 6; i++) l.push_back(A(20 + i, L"Many"));
        const bool more = t.step(game, l, 1, 0, ev);
        check(game.adds == 4 && more, "6 new pads: 4 in this step, wants another");
        t.step(game, l, 1, 50, ev);
        check(game.adds == 6 && game.pads.size() == 6, "the other 2 in the next step");
        std::vector<Tracker::View> v;
        t.view(v);
        check(v.size() == 6 && v[0].present && v[0].inGame, "view lists them as present and in game");
    }
}

// ------------------------------------------------------------------ fake input port (ESWC layout)
struct FakeDev { // IDirectInputDevice8W stand-in
    void** vtbl;
    GUID g;
    LONG refs;
    int unacquired;
    const DIDATAFORMAT* fmt;
    DWORD coop;
    DWORD buffer;
};
static ULONG STDMETHODCALLTYPE devRelease(FakeDev* d) { return static_cast<ULONG>(--d->refs); }
static HRESULT STDMETHODCALLTYPE devSetProperty(FakeDev* d, REFGUID, LPCDIPROPHEADER h) {
    d->buffer = reinterpret_cast<const DIPROPDWORD*>(h)->dwData;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE devUnacquire(FakeDev* d) {
    d->unacquired++;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE devSetDataFormat(FakeDev* d, LPCDIDATAFORMAT f) {
    d->fmt = f;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE devSetCoop(FakeDev* d, HWND, DWORD c) {
    d->coop = c;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE devGetInfo(FakeDev* d, LPDIDEVICEINSTANCEW inst) {
    if (inst->dwSize != sizeof(DIDEVICEINSTANCEW)) return E_INVALIDARG;
    inst->guidInstance = d->g;
    wcscpy_s(inst->tszProductName, L"Fake pad");
    return S_OK;
}
static void* g_devVtbl[32];
static FakeDev* newDev(const GUID& g) {
    FakeDev* d = static_cast<FakeDev*>(calloc(1, sizeof(FakeDev)));
    d->vtbl = g_devVtbl;
    d->g = g;
    d->refs = 1;
    return d;
}

struct FakeDI {
    void** vtbl;
    int creates;
};
static HRESULT STDMETHODCALLTYPE diCreateDevice(FakeDI* di, REFGUID g, LPDIRECTINPUTDEVICE8W* out, LPUNKNOWN) {
    di->creates++;
    *out = reinterpret_cast<LPDIRECTINPUTDEVICE8W>(newDev(g));
    return S_OK;
}
static void* g_diVtbl[16];
static FakeDI g_di;

static uintptr_t g_portVtbl[64];
static uintptr_t g_padVtbl[64];
static uintptr_t g_otherVtbl[64];
static DIDATAFORMAT g_fmt;
static uint8_t* g_port = nullptr;
static uint8_t g_cfg[0x100];
static bool g_enumFails = false;
static const uint8_t kSig[] = {0x11, 0x22, 0x33, 0x44};

static uint32_t& U32(uint8_t* p, size_t off) { return *reinterpret_cast<uint32_t*>(p + off); }
static void appendDevice(uint8_t* port, uint8_t* dev) {
    uint32_t n = U32(port, 0x2c);
    void** arr = static_cast<void**>(malloc((n + 1) * sizeof(void*)));
    if (n) memcpy(arr, reinterpret_cast<void*>(static_cast<uintptr_t>(U32(port, 0x30))), n * sizeof(void*));
    free(reinterpret_cast<void*>(static_cast<uintptr_t>(U32(port, 0x30))));
    arr[n] = dev;
    U32(port, 0x30) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arr));
    U32(port, 0x2c) = n + 1;
}
static uint8_t* newPad(const GUID& g, uint32_t lost) {
    uint8_t* pad = static_cast<uint8_t*>(calloc(1, 0x184));
    U32(pad, 0) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_padVtbl));
    U32(pad, 0x14) = lost;
    U32(pad, 0x34) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(newDev(g)));
    U32(pad, 0x30) = 0x80000000u | g.Data1; // device id (the game: hash of the instance GUID | 0x80000000)
    return pad;
}
static DWORD g_cbType = 0;
static wchar_t g_cbName[MAX_PATH];
static BOOL CALLBACK fakeEnumCb(const DIDEVICEINSTANCEW* inst, void* port) {
    g_cbType = inst->dwDevType;
    wcscpy_s(g_cbName, inst->tszProductName);
    if (!g_enumFails) appendDevice(static_cast<uint8_t*>(port), newPad(inst->guidInstance, 1));
    return DIENUM_CONTINUE;
}
static void __fastcall fakeReserve(uint32_t* arr, void*, uint32_t n) { // {count, ptr, cap} of 8-byte items
    if (static_cast<int>(n - arr[2]) <= 0) return;
    uint32_t* items = static_cast<uint32_t*>(malloc(n * 8));
    if (arr[0]) memcpy(items, reinterpret_cast<void*>(static_cast<uintptr_t>(arr[1])), arr[0] * 8);
    free(reinterpret_cast<void*>(static_cast<uintptr_t>(arr[1])));
    arr[1] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(items));
    arr[2] = n;
}
static uint8_t* newTable(uint32_t devices, uint32_t bindings) {
    uint8_t* t = static_cast<uint8_t*>(calloc(1, 0x38));
    uint32_t* ranges = reinterpret_cast<uint32_t*>(t + 0x14);
    fakeReserve(ranges, nullptr, devices);
    ranges[0] = devices;
    uint32_t* items = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(ranges[1]));
    for (uint32_t k = 0; k < devices; k++) { // all bindings on the last device
        items[k * 2] = k + 1 == devices ? 0 : 0;
        items[k * 2 + 1] = k + 1 == devices ? bindings : 0;
    }
    U32(t, 0x20) = bindings;
    return t;
}

static void buildPort() {
    g_devVtbl[2] = reinterpret_cast<void*>(&devRelease);
    g_devVtbl[6] = reinterpret_cast<void*>(&devSetProperty);
    g_devVtbl[8] = reinterpret_cast<void*>(&devUnacquire);
    g_devVtbl[11] = reinterpret_cast<void*>(&devSetDataFormat);
    g_devVtbl[13] = reinterpret_cast<void*>(&devSetCoop);
    g_devVtbl[15] = reinterpret_cast<void*>(&devGetInfo);
    g_diVtbl[3] = reinterpret_cast<void*>(&diCreateDevice);
    g_di.vtbl = g_diVtbl;
    g_port = static_cast<uint8_t*>(calloc(1, 0xac));
    U32(g_port, 0) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_portVtbl));
    U32(g_port, 0x94) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_di));
    U32(g_port, 0x98) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_cfg));
    // mouse, keyboard (not pads), one pad present at start
    for (int i = 0; i < 2; i++) {
        uint8_t* other = static_cast<uint8_t*>(calloc(1, 0x138));
        U32(other, 0) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_otherVtbl));
        appendDevice(g_port, other);
    }
    GUID a;
    memcpy(&a, G(1).b, 16);
    appendDevice(g_port, newPad(a, 0));
    // two binding tables of a running race (3 devices)
    uint8_t** tables = static_cast<uint8_t**>(malloc(2 * sizeof(void*)));
    tables[0] = newTable(3, 5);
    tables[1] = newTable(3, 0);
    U32(g_port, 0x14) = 2;
    U32(g_port, 0x18) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tables));
}

static uint8_t* g_portGlobal = nullptr;

static void backendTests() {
    printf("game backend (fake port)\n");
    buildPort();
    static const CodeSig code[] = {{reinterpret_cast<uintptr_t>(kSig), sizeof(kSig), kSig}};
    const uint8_t wrong[] = {0x11, 0x22, 0x33, 0x45};
    static CodeSig badCode[] = {{reinterpret_cast<uintptr_t>(kSig), sizeof(kSig), nullptr}};
    badCode[0].bytes = wrong;
    static PtrSig ptrs[1];
    g_portVtbl[0x2c] = 0x12345678;
    ptrs[0] = PtrSig{reinterpret_cast<uintptr_t>(&g_portVtbl[0x2c]), 0x12345678};
    GameTable t = {"fake",
                   0,
                   reinterpret_cast<uintptr_t>(&g_portGlobal),
                   reinterpret_cast<uintptr_t>(g_portVtbl),
                   reinterpret_cast<uintptr_t>(g_padVtbl),
                   reinterpret_cast<uintptr_t>(&fakeEnumCb),
                   reinterpret_cast<uintptr_t>(&fakeReserve),
                   reinterpret_cast<uintptr_t>(&g_fmt),
                   code,
                   1,
                   ptrs,
                   1};
    {
        GameTable bad = t;
        bad.code = badCode;
        GameBackend b(&bad, 0x400000, nullptr, nullptr);
        check(!b.ready() && b.failed(), "code bytes differ: backend stays off");
        PtrSig badPtr[1] = {{reinterpret_cast<uintptr_t>(&g_portVtbl[0x2c]), 0x1}};
        GameTable bad2 = t;
        bad2.ptrs = badPtr;
        GameBackend b2(&bad2, 0x400000, nullptr, nullptr);
        check(!b2.ready() && b2.failed(), "vtable entry differs: backend stays off");
        GameTable bad3 = t;
        bad3.code = nullptr;
        bad3.codeCount = 0;
        bad3.ptrs = nullptr;
        bad3.ptrCount = 0;
        bad3.enumCallback = 0x10; // never read: no port yet
        GameBackend b3(&bad3, 0x400000, nullptr, nullptr);
        check(!b3.ready() && !b3.failed(), "no port yet: not ready, not failed");
    }
    GameBackend b(&t, 0x400000, nullptr, nullptr);
    check(!b.ready(), "port global still 0: not ready");
    g_portGlobal = g_port;
    check(b.ready() && !b.failed(), "port set: ready");
    std::vector<Slot> s;
    b.slots(s);
    check(s.size() == 1 && s[0].index == 2 && sameGuid(s[0].instance, G(1)) && !s[0].lost, "slots: only the pad (index 2), its GUID");
    check(s[0].name == L"Fake pad" && b.devices() == 3, "slots: product name from the device; 3 input devices");

    // add B through the game's callback, with the type DirectInput reported (a wheel)
    Attached wheel = A(2, L"Pad B");
    wheel.devType = DI8DEVTYPE_DRIVING | DIDEVTYPE_HID;
    check(b.add(wheel), "add: true");
    check(g_cbType == (DI8DEVTYPE_DRIVING | DIDEVTYPE_HID) && wcscmp(g_cbName, L"Pad B") == 0,
          "the game's callback gets DirectInput's device type and the name");
    check(U32(g_port, 0x2c) == 4, "port has 4 devices");
    uint8_t** tables = reinterpret_cast<uint8_t**>(static_cast<uintptr_t>(U32(g_port, 0x18)));
    for (int k = 0; k < 2; k++) {
        uint8_t* tb = tables[k];
        const uint32_t* r = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(U32(tb, 0x18)));
        check(U32(tb, 0x14) == 4, k == 0 ? "table 1 has 4 ranges" : "table 2 has 4 ranges");
        check(r[6] == U32(tb, 0x20) && r[7] == 0, "new range: starts at the binding count, empty");
        check(r[4] == 0 && r[5] == U32(tb, 0x20), "old ranges unchanged");
    }
    b.slots(s);
    check(s.size() == 2 && s[1].index == 3 && sameGuid(s[1].instance, G(2)) && s[1].lost, "slots: B at index 3, not acquired yet");

    // the callback adds nothing (CreateDevice failed in the game)
    g_enumFails = true;
    check(!b.add(A(3, L"Pad C")) && U32(g_port, 0x2c) == 4, "callback added nothing: false, tables untouched");
    check(GET_DIDEVICE_TYPE(g_cbType) == DI8DEVTYPE_GAMEPAD && (g_cbType & DIDEVTYPE_HID), "type unknown: a HID gamepad");
    g_enumFails = false;

    // refresh slot 3
    uint8_t* padB = reinterpret_cast<uint8_t**>(static_cast<uintptr_t>(U32(g_port, 0x30)))[3];
    FakeDev* old = reinterpret_cast<FakeDev*>(static_cast<uintptr_t>(U32(padB, 0x34)));
    U32(padB, 0x14) = 0;
    check(b.refresh(3, A(2, L"Pad B")), "refresh: true");
    FakeDev* fresh = reinterpret_cast<FakeDev*>(static_cast<uintptr_t>(U32(padB, 0x34)));
    check(fresh != old && old->unacquired == 1 && old->refs == 0, "old device unacquired and released");
    check(fresh->fmt == &g_fmt && fresh->coop == (DISCL_NONEXCLUSIVE | DISCL_FOREGROUND) && fresh->buffer == 0x100,
          "fresh device set up like the game (pad format, non-exclusive foreground, buffer 256)");
    check(U32(padB, 0x14) == 1 && g_di.creates == 1, "slot marked for the game's own Acquire; one CreateDevice");
    g_cfg[0x84] = 1;
    check(b.refresh(3, A(2, L"Pad B")), "refresh with exclusive config");
    fresh = reinterpret_cast<FakeDev*>(static_cast<uintptr_t>(U32(padB, 0x34)));
    check(fresh->coop == (DISCL_EXCLUSIVE | DISCL_FOREGROUND), "exclusive config: exclusive like the game");
    g_cfg[0x84] = 0;
    check(!b.refresh(1, A(2, L"x")), "refresh of the keyboard slot: refused");
    check(!b.refresh(9, A(2, L"x")), "refresh of a slot out of range: refused");
    b.slots(s);
    check(s.size() == 2 && sameGuid(s[1].instance, G(2)), "slot keeps its GUID after the refresh");

    // tracker + backend: plug, unplug, replug -> one registration, same slot
    Tracker tr;
    std::vector<Event> ev;
    std::vector<Attached> l = {A(1, L"Start"), A(2, L"Pad B")};
    tr.step(b, l, 1, 0, ev);
    check(ev.empty() && U32(g_port, 0x2c) == 4, "tracker on the fake port: both known, nothing added");
    l.push_back(A(4, L"Pad D"));
    tr.step(b, l, 2, 100, ev);
    check(U32(g_port, 0x2c) == 5 && countNote(ev, Note::Added) == 1, "Pad D registered through the callback");
    tr.step(b, {A(1, L"Start"), A(2, L"Pad B")}, 3, 200, ev);
    tr.step(b, l, 4, 300, ev);
    check(U32(g_port, 0x2c) == 5 && countNote(ev, Note::Back) == 1, "Pad D replugged: same slot, no second device");
    for (int i = 0; i < 1000 && U32(g_port, 0x2c) < GameBackend::kMaxDevices; i++) {
        GUID g;
        memcpy(&g, G(500 + i).b, 16);
        appendDevice(g_port, newPad(g, 0));
    }
    check(!b.add(A(99, L"Too many")), "device limit: refused");
}

static void realTableTests() {
    printf("real tables in a process that is not the game\n");
    const GameTable* tables[3] = {&eswcTable(), &sunriseTable(), &originalTable()};
    for (const GameTable* t : tables) {
        GameBackend b(t, 0x400000, nullptr, nullptr);
        std::vector<Slot> s;
        b.slots(s);
        check(!b.ready() && b.failed() && s.empty() && !b.add(A(1, L"x")) && !b.refresh(2, A(1, L"x")),
              "code bytes do not match here: off, no crash, no game call");
        check(t->bind != nullptr && t->bind().codeCount >= 14, "bindings table with its code signatures");
        // the race is CConstructionGame.CurrentRace (the host's reflection: ESWC +0x140, Sunrise/Original +0x12c);
        // 1.2.0 read CTrackMania.Races (+0x2f0/+0x2a8), so the running race table was never found
        check(t->bind().appRace == (t == &eswcTable() ? 0x140u : 0x12cu), "race field = CurrentRace");
        Binder bd(&t->bind(), 0x400000, t->reserve8, nullptr, nullptr);
        uint8_t fakePort[0xac] = {};
        bd.apply(fakePort, 2, L"x");
        check(!bd.verify() && !bd.last().ok, "bindings code does not match here: off, no game call");
    }
    check(tableForTimestamp(0x5ce7272f) == &eswcTable() && tableForTimestamp(0x5cc8f0cf) == &sunriseTable() &&
              tableForTimestamp(0x5d337477) == &originalTable() && !tableForTimestamp(1),
          "exe timestamps pick the tables");
}

// ------------------------------------------------------------------ enumeration logic (worker)
static Guid hidProduct(uint16_t vid, uint16_t pid) {
    Guid g;
    memset(g.b, 0, 16);
    g.b[0] = static_cast<uint8_t>(vid);
    g.b[1] = static_cast<uint8_t>(vid >> 8);
    g.b[2] = static_cast<uint8_t>(pid);
    g.b[3] = static_cast<uint8_t>(pid >> 8);
    memcpy(g.b + 10, "PIDVID", 6);
    return g;
}

static void enumTests() {
    printf("enumeration logic\n");
    {
        Schedule s;
        check(!s.pending() && !s.take(0) && s.waitMs(0) == 0xFFFFFFFFu, "schedule: idle");
        s.trigger(1000);
        check(s.pending() && s.waitMs(1000) == 500 && !s.take(1499), "after a change: first list 0.5 s later");
        check(s.take(1500) && s.waitMs(1500) == 1500, "0.5 s: list, next at 2 s");
        check(!s.take(2999) && s.take(3000), "2 s: list");
        check(s.take(20000) && !s.pending(), "late wake-up: the steps that are due give one list, series done");
        s.trigger(0xFFFFFF00u); // tick count wraps
        check(s.take(0xFFFFFF00u + 500) && s.waitMs(0xFFFFFF00u + 500) == 1500, "schedule across the tick count wrap");
        s.trigger(5000);
        s.take(5500);
        s.trigger(7000); // another change during a series starts it over
        check(s.waitMs(7000) == 500, "a new change restarts the series");
        int lists = 0;
        for (uint32_t t = 7000; t <= 30000; t += 100)
            if (s.take(t)) lists++;
        check(lists == Schedule::kSteps && Schedule::kAfterMs[Schedule::kSteps - 1] >= 10000,
              "one series = 4 lists, the last 10 s after the change");
    }
    {
        uint16_t vid = 0, pid = 0;
        check(productIds(hidProduct(0x045e, 0x02ff), vid, pid) && vid == 0x045e && pid == 0x02ff, "VID/PID from a HID product GUID");
        check(!productIds(G(7), vid, pid), "not a HID product GUID: no VID/PID");
        check(strcmp(typeName(DI8DEVTYPE_GAMEPAD | (2 << 8) | DIDEVTYPE_HID), "gamepad") == 0 &&
                  strcmp(typeName(DI8DEVTYPE_DRIVING), "wheel") == 0 && strcmp(typeName(DI8DEVTYPE_SUPPLEMENTAL), "supplemental") == 0 &&
                  strcmp(typeName(0), "controller") == 0,
              "device type words");
        Attached x{G(0x12345678), hidProduct(0x045e, 0x02ff), L"Controller (Xbox One For Windows)", DI8DEVTYPE_GAMEPAD | DIDEVTYPE_HID, true};
        check(describe(x) == "Controller (Xbox One For Windows) (gamepad, VID 045E PID 02FF, XInput, id 12345678)", "describe: Xbox pad");
        Attached w{G(5), G(6), L"", DI8DEVTYPE_DRIVING, false};
        check(describe(w) == "(no name) (wheel, id 00000005)", "describe: no name, not HID");
        Attached u{G(9), G(10), L"Pad \u00e4", DI8DEVTYPE_JOYSTICK, false};
        check(describe(u).find("Pad \xC3\xA4 (joystick") == 0, "describe: names in UTF-8");
    }
    {
        const Diff d = diff({A(1, L"a"), A(2, L"b")}, {A(2, L"b"), A(3, L"c")});
        check(d.added.size() == 1 && sameGuid(d.added[0].instance, G(3)) && d.gone.size() == 1 && sameGuid(d.gone[0].instance, G(1)),
              "diff: one new, one gone");
        check(diff({A(1, L"a")}, {A(1, L"a")}).added.empty(), "diff: same list, nothing");
    }
    {
        check(isXInputPath(L"\\\\?\\hid#vid_045e&pid_02ff&ig_00#8&39e3c7f0&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}"),
              "XInput HID path (IG_)");
        check(!isXInputPath(L"\\\\?\\hid#vid_046d&pid_c08b&mi_01&col02#8&54eee94&0&0001#{4d1e55b2-f16f-11cf-88cb-001111000030}") &&
                  !isXInputPath(L"\\\\?\\hid#vid_1234&pid_big_00#x") && !isXInputPath(nullptr),
              "other HID paths are not XInput");
        check(idsFromPath(L"\\\\?\\HID#VID_045E&PID_02FF&IG_00#8&39e3") == "VID 045E PID 02FF" &&
                  idsFromPath(L"\\\\?\\hid#vid_046d&pid_c08b&mi_01") == "VID 046D PID C08B" && idsFromPath(L"\\\\?\\root#x").empty(),
              "VID/PID from a device path");
    }
    {
        Environment e;
        check(diagnose(e) == Hint::None, "nothing plugged in: no hint");
        e.xboxUsb = 1; // the case of the report: Xbox pad on USB, switched off -> no game controller device
        e.xinputKnown = true;
        check(diagnose(e) == Hint::XboxOff, "Xbox pad on USB, no XInput pad, no DirectInput device: switched off");
        e.lists.push_back(Attached{G(1), hidProduct(0x045e, 0x02ff), L"Controller (Xbox One For Windows)", DI8DEVTYPE_GAMEPAD, true});
        e.xinputMask = 1;
        check(diagnose(e) == Hint::None, "switched on: DirectInput lists it, no hint");
        e.lists.clear();
        check(diagnose(e) == Hint::XInputNotListed, "XInput pad that DirectInput does not list");
        e.xinputKnown = false;
        e.xboxUsb = 0;
        check(diagnose(e) == Hint::None, "XInput unknown: no XInput hint");
        Environment g;
        g.lists.push_back(A(3, L"Generic pad"));
        g.xinputKnown = true;
        check(diagnose(g) == Hint::None, "generic HID pad, no Xbox: no hint");
        check(hintText(Hint::XboxOff)[0] != 0 && hintText(Hint::None)[0] == 0, "hint texts");
    }
    {
        // the tracker hands DirectInput's type to the game
        FakeGame game;
        Tracker t;
        std::vector<Event> ev;
        Attached wheel = A(3, L"Wheel");
        wheel.devType = DI8DEVTYPE_DRIVING;
        t.step(game, {wheel}, 1, 0, ev);
        check(game.adds == 1 && game.lastAddType == DI8DEVTYPE_DRIVING && game.devices() == 3, "tracker: add with the device type");
        check(game.pads.size() == 1 && game.pads[0].name == L"Wheel", "fake game: slot has the name");
    }
}

// ------------------------------------------------------------------ bindings for a late pad (fake app/config/race)
// Laid out like ESWC: app {+cb SetDefaults, +0x58 port, +race}, race {+app, +port, +config table, +all table},
// config {+0x14/+0x18 entries (action index, device id, object), +0x20/+0x24 actions, +0x2c/+0x30 ids, +0x4c port}.
static uint32_t __fastcall fakePadObject(uint8_t*, void*, uint32_t kind, uint32_t index) {
    return kind == 7 && index == 1 ? 0xffffffffu : kind * 0x100 + index; // no second POV: the game skips it
}
static uint8_t g_bApp[0x400];
static uint8_t g_bRace[0x200];
static uint8_t g_bCfg[0x60];
static uint32_t g_actDesc[10][4];  // the race action descs
static uint32_t g_menuDesc[5][4];  // the "all objects" action descs
static uintptr_t g_actionList[10]; // globals holding the race action descs (game order)
static uintptr_t g_allAct[5];      // globals holding the menu action descs
static uintptr_t g_cfgVt[4], g_allVt[4], g_otherTblVt[4];
static uint8_t* g_bPort = nullptr;
static uint8_t* g_bPortGlobal = nullptr;
static int g_passCalls = 0;
static int g_curCalls = 0;
static void fakeSetDefaults() {}

static void cfgAdd(uint32_t action, uint32_t id, uint32_t object) {
    uint32_t n = U32(g_bCfg, 0x14);
    uint32_t* e = static_cast<uint32_t*>(realloc(reinterpret_cast<void*>(static_cast<uintptr_t>(U32(g_bCfg, 0x18))), (n + 1) * 12));
    e[n * 3] = action;
    e[n * 3 + 1] = id;
    e[n * 3 + 2] = object;
    U32(g_bCfg, 0x18) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e));
    U32(g_bCfg, 0x14) = n + 1;
    uint32_t k = U32(g_bCfg, 0x2c);
    uint32_t* ids = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(U32(g_bCfg, 0x30)));
    for (uint32_t i = 0; i < k; i++)
        if (ids[i] == id) return;
    ids = static_cast<uint32_t*>(realloc(ids, (k + 1) * 4));
    ids[k] = id;
    U32(g_bCfg, 0x30) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ids));
    U32(g_bCfg, 0x2c) = k + 1;
}
static bool cfgKnows(uint32_t id) {
    uint32_t* ids = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(U32(g_bCfg, 0x30)));
    for (uint32_t i = 0; i < U32(g_bCfg, 0x2c); i++)
        if (ids[i] == id) return true;
    return false;
}
static uintptr_t __fastcall fakeCurConfig(uint8_t* app, void*) {
    g_curCalls++;
    return app == g_bApp ? reinterpret_cast<uintptr_t>(g_bCfg) : 0;
}
// the game's defaults pass: every pad without bindings gets Accelerate=button 0, Brake=button 1, Steer=X axis
static void __fastcall fakePass(uint8_t*, void*, uint8_t* config) {
    g_passCalls++;
    if (config != g_bCfg) return;
    const uint32_t n = U32(g_bPort, 0x2c);
    uint8_t** devs = reinterpret_cast<uint8_t**>(static_cast<uintptr_t>(U32(g_bPort, 0x30)));
    for (uint32_t i = 0; i < n; i++) {
        if (U32(devs[i], 0) != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_padVtbl))) continue;
        const uint32_t id = U32(devs[i], 0x30);
        if (cfgKnows(id)) continue;
        cfgAdd(3, id, fakePadObject(devs[i], nullptr, 8, 0));
        cfgAdd(4, id, fakePadObject(devs[i], nullptr, 8, 1));
        cfgAdd(8, id, fakePadObject(devs[i], nullptr, 0, 0));
    }
}
static BindTable g_fakeBind;
static const BindTable& fakeBind() { return g_fakeBind; }

static uint8_t* newTableVt(uintptr_t* vt, uint32_t devices, uint32_t bindings) {
    uint8_t* t = newTable(devices, bindings);
    U32(t, 0) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(vt));
    if (bindings) { // bindings on the last device, (action, object) pairs
        U32(t, 0x20) = 0; // newTable only set the count
        fakeReserve(reinterpret_cast<uint32_t*>(t + 0x20), nullptr, bindings);
        U32(t, 0x20) = bindings;
        uint32_t* b = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(U32(t, 0x24)));
        for (uint32_t k = 0; k < bindings; k++) {
            b[k * 2] = 0x1000 + k;
            b[k * 2 + 1] = k;
        }
    }
    U32(t, 0x2c) = 1;
    U32(t, 0x30) = 1;
    U32(t, 0x34) = 0x12345678; // a stale cursor
    return t;
}
// the game's table lookup (0x460670): bindings of a device index
static int tableRange(uint8_t* t, uint32_t dev, const uint32_t** items) {
    const uint32_t* r = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(U32(t, 0x18)));
    *items = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(U32(t, 0x24))) + r[dev * 2] * 2;
    return static_cast<int>(r[dev * 2 + 1]);
}

static uint8_t *g_tRace, *g_tAll, *g_tOther;
static void buildBindWorld() {
    g_padVtbl[0x94 / 4] = reinterpret_cast<uintptr_t>(&fakePadObject);
    g_bPort = static_cast<uint8_t*>(calloc(1, 0xac));
    U32(g_bPort, 0) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_portVtbl));
    U32(g_bPort, 0x94) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_di));
    U32(g_bPort, 0x98) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_cfg));
    for (int i = 0; i < 2; i++) { // mouse, keyboard
        uint8_t* other = static_cast<uint8_t*>(calloc(1, 0x138));
        U32(other, 0) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_otherVtbl));
        U32(other, 0x30) = 0x80000010u + i;
        appendDevice(g_bPort, other);
    }
    // a race is running: its config table (keyboard bindings), its "all objects" table, and some other table
    g_tRace = newTableVt(g_cfgVt, 2, 6);
    g_tAll = newTableVt(g_allVt, 2, 4);
    g_tOther = newTableVt(g_otherTblVt, 2, 2);
    uint8_t** tables = static_cast<uint8_t**>(malloc(3 * sizeof(void*)));
    tables[0] = g_tOther;
    tables[1] = g_tRace;
    tables[2] = g_tAll;
    U32(g_bPort, 0x14) = 3;
    U32(g_bPort, 0x18) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tables));
    // config: actions, keyboard bindings
    static uintptr_t actions[10];
    for (int k = 0; k < 10; k++) {
        actions[k] = reinterpret_cast<uintptr_t>(&g_actDesc[k]);
        g_actionList[k] = actions[k];
    }
    for (int k = 0; k < 5; k++) g_allAct[k] = reinterpret_cast<uintptr_t>(&g_menuDesc[k]);
    U32(g_bCfg, 0x20) = 10;
    U32(g_bCfg, 0x24) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(actions));
    U32(g_bCfg, 0x4c) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_bPort));
    cfgAdd(3, 0x80000011u, 0x7f); // keyboard: Accelerate = up
    // app and race
    U32(g_bApp, 0x258) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&fakeSetDefaults));
    U32(g_bApp, 0x58) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_bPort));
    U32(g_bApp, 0x140) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_bRace));
    U32(g_bRace, 0x40) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_bApp));
    U32(g_bRace, 0x140) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_bPort));
    U32(g_bRace, 0x144) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_tRace));
    U32(g_bRace, 0x148) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_tAll));
    static uint8_t* appGlobal = g_bApp;
    BindTable b = {};
    b.appGlobal = reinterpret_cast<uintptr_t>(&appGlobal);
    b.appDefaultsCb = 0x258;
    b.appPort = 0x58;
    b.appRace = 0x140; // ESWC CConstructionGame.CurrentRace
    b.setDefaults = reinterpret_cast<uintptr_t>(&fakeSetDefaults);
    b.defaultsPass = reinterpret_cast<uintptr_t>(&fakePass);
    b.currentConfig = reinterpret_cast<uintptr_t>(&fakeCurConfig);
    b.raceApp = 0x40;
    b.racePort = 0x140;
    b.raceConfigTable = 0x144;
    b.raceAllTable = 0x148;
    b.configTableVtbl = reinterpret_cast<uintptr_t>(g_cfgVt);
    b.allTableVtbl = reinterpret_cast<uintptr_t>(g_allVt);
    for (int k = 0; k < 5; k++) b.allActions[k] = reinterpret_cast<uintptr_t>(&g_allAct[k]);
    b.actionList = reinterpret_cast<uintptr_t>(g_actionList);
    g_fakeBind = b;
}

static std::vector<std::string> g_bindLog;
static void bindLog(void*, int, const char* text) { g_bindLog.push_back(text); }
static bool bindLogHas(const char* part) {
    for (const auto& l : g_bindLog)
        if (l.find(part) != std::string::npos) return true;
    return false;
}

static void bindTests() {
    printf("bindings for a late pad (fake app, config, race)\n");
    buildBindWorld();
    static const CodeSig code[] = {{reinterpret_cast<uintptr_t>(kSig), sizeof(kSig), kSig}};
    GameTable t = {"fake", 0, reinterpret_cast<uintptr_t>(&g_bPortGlobal), reinterpret_cast<uintptr_t>(g_portVtbl),
                   reinterpret_cast<uintptr_t>(g_padVtbl), reinterpret_cast<uintptr_t>(&fakeEnumCb),
                   reinterpret_cast<uintptr_t>(&fakeReserve), reinterpret_cast<uintptr_t>(&g_fmt), code, 1, nullptr, 0, &fakeBind};
    g_bPortGlobal = g_bPort;
    GameBackend b(&t, 0x400000, &bindLog, nullptr);
    check(b.ready() && b.binder() != nullptr, "backend with bindings: ready");
    check(bindLogHas("bindings code checked"), "bindings code checked at start");

    // 1: a new pad during a race, no saved bindings
    const uint32_t raceBinds = U32(g_tRace, 0x20), allBinds = U32(g_tAll, 0x20), otherBinds = U32(g_tOther, 0x20);
    check(b.add(A(7, L"Pad Seven")), "pad added");
    uint8_t* pad = reinterpret_cast<uint8_t**>(static_cast<uintptr_t>(U32(g_bPort, 0x30)))[2];
    const uint32_t id = U32(pad, 0x30);
    const Binder::Result& r = b.binder()->last();
    check(r.ok && !r.saved && r.configPairs == 3 && g_passCalls == 1, "no saved bindings: the game's defaults pass ran, 3 bindings");
    check(cfgKnows(id) && U32(g_bCfg, 0x14) == 4, "config: the pad's id and 3 entries added, keyboard entry kept");
    const uint32_t* it = nullptr;
    check(tableRange(g_tRace, 2, &it) == 3 && U32(g_tRace, 0x20) == raceBinds + 3, "race table: 3 bindings for the pad");
    check(it[0] == reinterpret_cast<uintptr_t>(&g_actDesc[3]) && it[1] == 0x800 && it[2] == reinterpret_cast<uintptr_t>(&g_actDesc[4]) &&
              it[3] == 0x801 && it[4] == reinterpret_cast<uintptr_t>(&g_actDesc[8]) && it[5] == 0,
          "race table: Accelerate=button 0, Brake=button 1, Steer=X axis (action descs from the config)");
    check(tableRange(g_tRace, 1, &it) == 6 && it[0] == 0x1000, "race table: the keyboard's range unchanged");
    check(U32(g_tRace, 0x2c) == 0 && U32(g_tRace, 0x30) == 0xffffffffu && U32(g_tRace, 0x34) == U32(g_tRace, 0x24),
          "race table: lookup cursor reset (no stale pointer into the old bindings)");
    check(tableRange(g_tAll, 2, &it) == 9 && U32(g_tAll, 0x20) == allBinds + 9 && r.menuAdded == 9 && r.menuTables == 1,
          "all objects table: 9 menu bindings (the missing second POV is skipped like the game does)");
    check(it[0] == reinterpret_cast<uintptr_t>(&g_menuDesc[0]) && it[1] == 0x800 && it[16] == reinterpret_cast<uintptr_t>(&g_menuDesc[4]) &&
              it[17] == 0x101,
          "all objects table: button 0 -> menu action 0 ... Y axis -> menu action 4");
    check(U32(g_tOther, 0x20) == otherBinds && tableRange(g_tOther, 2, &it) == 0, "other table: only the empty range");
    check(r.raceAdded == 3 && bindLogHas("the game's defaults added to the profile, 3 binding(s): Accelerate=800, Brake=801, Steer=0"),
          "log names the bindings");

    // 2: saved bindings + race running (the in-game case of 1.2.0): kept, applied to the running race at once
    const uint32_t savedId = 0x80000000u | 8; // newPad's id for GUID 8
    cfgAdd(2, savedId, 0x803); // Respawn = button 3
    cfgAdd(3, savedId, 0x805); // Accelerate = button 5
    const int passBefore = g_passCalls;
    check(b.add(A(8, L"Pad Eight")), "second pad added");
    const Binder::Result& r2 = b.binder()->last();
    check(r2.ok && r2.saved && r2.configPairs == 2 && g_passCalls == passBefore, "saved bindings: kept, no defaults pass");
    check(tableRange(g_tRace, 3, &it) == 2 && it[0] == reinterpret_cast<uintptr_t>(&g_actDesc[2]) && it[1] == 0x803 && it[3] == 0x805,
          "race table: the saved Respawn=button 3, Accelerate=button 5");
    check(bindLogHas("saved in the profile, kept, 2 binding(s): Respawn=803, Accelerate=805"), "log: saved bindings");

    // 3: outside a race: the config and the menu tables only
    U32(g_bApp, 0x140) = 0;
    const uint32_t raceNow = U32(g_tRace, 0x20);
    check(b.add(A(9, L"Pad Nine")), "third pad added in the menu");
    const Binder::Result& r3 = b.binder()->last();
    check(r3.ok && r3.raceAdded == 0 && r3.configPairs == 3 && r3.menuAdded == 9 && U32(g_tRace, 0x20) == raceNow,
          "no race: defaults in the config (for the next race start), menu table updated, race table untouched");
    U32(g_bApp, 0x140) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_bRace));

    // 4: the app is not the one expected: nothing is changed
    U32(g_bApp, 0x258) = 0x1234;
    const uint32_t cfgN = U32(g_bCfg, 0x14);
    check(b.add(A(10, L"Pad Ten")), "fourth pad added (registration does not depend on bindings)");
    check(!b.binder()->last().ok && U32(g_bCfg, 0x14) == cfgN && bindLogHas("is not as expected"),
          "app callback differs: config and tables untouched, logged");
    U32(g_bApp, 0x258) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&fakeSetDefaults));

    // 4b: a race that does not use this port (wrong race field): nothing appended, the reason is logged
    {
        static uint8_t otherRace[0x200];
        U32(otherRace, 0x140) = 0x1234;
        U32(g_bApp, 0x140) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(otherRace));
        g_bindLog.clear();
        const uint32_t raceBefore = U32(g_tRace, 0x20);
        check(b.add(A(11, L"Pad Eleven")), "pad added while the race field holds another object");
        check(b.binder()->last().ok && b.binder()->last().raceAdded == 0 && U32(g_tRace, 0x20) == raceBefore &&
                  bindLogHas("its input port is 00001234"),
              "race not using the port: race table untouched, reason logged");
        U32(g_bApp, 0x140) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_bRace));
    }

    // 4c: the next race start: the game builds a new race table from the config; the check logs the late pads
    {
        g_bindLog.clear();
        check(b.poll(), "late pads: the backend wants steps for the race start check");
        static uint8_t race2[0x200];
        const uint32_t devs = U32(g_bPort, 0x2c);
        uint8_t* nt = newTableVt(g_cfgVt, devs, 0);
        uint32_t* rr = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(U32(nt, 0x18)));
        rr[2 * 2 + 1] = 3; // the game put Pad Seven's 3 config bindings into the new race table
        uint8_t** old = reinterpret_cast<uint8_t**>(static_cast<uintptr_t>(U32(g_bPort, 0x18)));
        uint8_t** tl = static_cast<uint8_t**>(malloc(4 * sizeof(void*)));
        memcpy(tl, old, 3 * sizeof(void*));
        tl[3] = nt;
        U32(g_bPort, 0x18) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tl));
        U32(g_bPort, 0x14) = 4;
        U32(race2, 0x40) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_bApp));
        U32(race2, 0x140) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_bPort));
        U32(race2, 0x144) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(nt));
        U32(g_bApp, 0x140) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(race2));
        b.poll();
        check(bindLogHas("race start: Pad Seven (device 2) has 3 binding(s) in the race table") &&
                  bindLogHas("race start: Pad Eight (device 3) has 0 binding(s)"),
              "race start check: logs each late pad's bindings in the new race table");
        g_bindLog.clear();
        b.poll();
        check(g_bindLog.empty(), "same race: logged once");
        U32(g_bApp, 0x140) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_bRace));
    }

    // 5: appendToTable refuses what the game's AddBinding would not produce for the last device
    BindPair p[1] = {{0x55, 0x66}};
    const char* why = "";
    uint8_t* t2 = newTableVt(g_cfgVt, 3, 0);
    check(!appendToTable(t2, 1, p, 1, reinterpret_cast<uintptr_t>(&fakeReserve), &why) && strstr(why, "last device"),
          "append: not the last device -> refused");
    check(appendToTable(t2, 2, p, 1, reinterpret_cast<uintptr_t>(&fakeReserve), &why), "append: last device ok");
    check(!appendToTable(t2, 2, p, 1, reinterpret_cast<uintptr_t>(&fakeReserve), &why) && strstr(why, "already"),
          "append twice -> refused (no duplicates)");
    uint8_t* t3 = newTableVt(g_cfgVt, 3, 0);
    reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(U32(t3, 0x18)))[4] = 5; // range start not at the end
    check(!appendToTable(t3, 2, p, 1, reinterpret_cast<uintptr_t>(&fakeReserve), &why) && strstr(why, "end"),
          "append: range not at the end -> refused");
    check(actionName(3) == "Accelerate" && actionName(8) == "Steer" && actionName(20) == "action 20", "action names");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    realTableTests();
    trackerTests();
    enumTests();
    backendTests();
    bindTests();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
