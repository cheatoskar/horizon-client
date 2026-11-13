#define DIRECTINPUT_VERSION 0x0800
#include "hp_watch.h"

#include <dbt.h>
#include <dinput.h>
#include <setupapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

namespace hp {

namespace {
// GUID_DEVINTERFACE_HID {4D1E55B2-F16F-11CF-88CB-001111000030}
const GUID kHidInterface = {0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
// setup class "XboxComposite" {05F5CFE2-4733-4950-A6BB-07AAD01A3A84}: an Xbox controller on USB or Bluetooth,
// present even while the pad is switched off (its game controller device is a child that comes and goes)
const GUID kXboxComposite = {0x05f5cfe2, 0x4733, 0x4950, {0xa6, 0xbb, 0x07, 0xaa, 0xd0, 0x1a, 0x3a, 0x84}};
constexpr uint32_t kPollMs = 3000; // without notifications
constexpr uint32_t kXInputMs = 1000;
constexpr uint32_t kTickMs = 1000;
constexpr uint32_t kFakeMs = 100;
constexpr uint32_t kLogWindowMs = 10000; // at most kLogLines HID arrival/removal lines per 10 s
constexpr int kLogLines = 10;
constexpr DWORD kXInputFlagGamepad = 1;

bool due(uint32_t now, uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }

using Create8Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

BOOL CALLBACK collect(LPCDIDEVICEINSTANCEW inst, LPVOID ref) {
    auto* out = static_cast<std::vector<Attached>*>(ref);
    Attached a;
    memcpy(a.instance.b, &inst->guidInstance, 16);
    memcpy(a.product.b, &inst->guidProduct, 16);
    a.name = inst->tszProductName[0] ? inst->tszProductName : inst->tszInstanceName;
    a.devType = inst->dwDevType;
    out->push_back(a);
    return DIENUM_CONTINUE;
}

bool sameSet(const std::vector<Attached>& a, const std::vector<Attached>& b) {
    if (a.size() != b.size()) return false;
    for (const auto& x : a) {
        bool found = false;
        for (const auto& y : b)
            if (sameGuid(x.instance, y.instance)) found = true;
        if (!found) return false;
    }
    return true;
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DEVICECHANGE && (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE)) {
        auto* w = reinterpret_cast<Watcher*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (w) w->onDeviceChange(wp, lp);
        return TRUE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HMODULE selfModule() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&wndProc), &m);
    return m;
}

HMODULE loadSystemDll(const wchar_t* name) {
    wchar_t path[MAX_PATH];
    const UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n >= MAX_PATH - 32) return nullptr;
    wcscat_s(path, L"\\");
    wcscat_s(path, name);
    return LoadLibraryW(path);
}

std::string slotList(uint32_t mask) {
    std::string s;
    for (int i = 0; i < 4; i++)
        if (mask & (1u << i)) {
            if (!s.empty()) s += ", ";
            s += static_cast<char>('1' + i);
        }
    return s;
}
} // namespace

void Watcher::note(const char* fmt, ...) {
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    AcquireSRWLockExclusive(&lock_);
    if (notes_.size() < 128) notes_.push_back(buf);
    ReleaseSRWLockExclusive(&lock_);
}

bool Watcher::takeNotes(std::vector<std::string>& out) {
    AcquireSRWLockExclusive(&lock_);
    out.swap(notes_);
    notes_.clear();
    ReleaseSRWLockExclusive(&lock_);
    return !out.empty();
}

uint32_t Watcher::list(std::vector<Attached>& out) {
    AcquireSRWLockShared(&lock_);
    out = list_;
    const uint32_t g = gen_;
    ReleaseSRWLockShared(&lock_);
    return g;
}

bool Watcher::start(KickFn kick, void* user, bool fake) {
    if (thread_) return true;
    kick_ = kick;
    user_ = user;
    fake_ = fake;
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_) return false;
    thread_ = CreateThread(nullptr, 0, &Watcher::threadMain, this, 0, nullptr);
    if (!thread_) {
        CloseHandle(stop_);
        stop_ = nullptr;
        return false;
    }
    return true;
}

void Watcher::stop() {
    if (!thread_) return;
    SetEvent(stop_);
    WaitForSingleObject(thread_, 10000);
    CloseHandle(thread_);
    CloseHandle(stop_);
    thread_ = nullptr;
    stop_ = nullptr;
}

DWORD WINAPI Watcher::threadMain(LPVOID self) {
    static_cast<Watcher*>(self)->run();
    return 0;
}

void Watcher::onDeviceChange(WPARAM what, LPARAM data) {
    const uint32_t now = GetTickCount();
    schedule_.trigger(now);
    changed_ = true;
    if (due(now, logWindow_ + kLogWindowMs)) {
        logWindow_ = now;
        logCount_ = 0;
    }
    if (logCount_ > kLogLines) return;
    if (logCount_++ == kLogLines) {
        note("more HID device changes in the next seconds are not logged");
        return;
    }
    const auto* hdr = reinterpret_cast<const DEV_BROADCAST_HDR*>(data);
    std::string ids;
    bool xinput = false;
    if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
        const auto* di = reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(data);
        ids = idsFromPath(di->dbcc_name);
        xinput = isXInputPath(di->dbcc_name);
    }
    note("HID device %s: %s%s", what == DBT_DEVICEARRIVAL ? "arrived" : "removed", ids.empty() ? "(no VID/PID)" : ids.c_str(),
         xinput ? ", XInput" : "");
}

bool Watcher::xinputOf(const GUID& instance) {
    Guid g;
    memcpy(g.b, &instance, 16);
    for (const auto& p : paths_)
        if (sameGuid(p.g, g)) return p.xinput;
    bool xi = false;
    IDirectInputDevice8W* d = nullptr;
    if (di_ && SUCCEEDED(static_cast<IDirectInput8W*>(di_)->CreateDevice(instance, &d, nullptr)) && d) {
        DIPROPGUIDANDPATH p = {};
        p.diph.dwSize = sizeof(p);
        p.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        p.diph.dwHow = DIPH_DEVICE;
        if (SUCCEEDED(d->GetProperty(DIPROP_GUIDANDPATH, &p.diph))) {
            p.wszPath[MAX_PATH - 1] = 0;
            xi = isXInputPath(p.wszPath);
        }
        d->Release(); // never acquired: the device is only asked for its path
    }
    if (paths_.size() < 64) paths_.push_back(PathEntry{g, xi});
    return xi;
}

void Watcher::countXbox() {
    int n = 0;
    HDEVINFO set = SetupDiGetClassDevsW(&kXboxComposite, nullptr, nullptr, DIGCF_PRESENT);
    if (set != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA dd = {};
        dd.cbSize = sizeof(dd);
        for (DWORD k = 0; k < 64 && SetupDiEnumDeviceInfo(set, k, &dd); k++) {
            wchar_t id[256] = L"";
            if (!SetupDiGetDeviceInstanceIdW(set, &dd, id, 256, nullptr)) continue;
            if (_wcsnicmp(id, L"SWD\\", 4) == 0) continue; // the synthetic pad of Windows' input injection, not a real one
            n++;
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    if (n != xboxUsb_) note("Xbox controllers plugged in (USB or Bluetooth): %d", n);
    xboxUsb_ = n;
}

void Watcher::updateHint() {
    Environment e;
    e.xboxUsb = xboxUsb_;
    e.xinputMask = xinputMask_;
    e.xinputKnown = xinputCaps_ != nullptr;
    e.lists = list_; // only this thread writes list_
    const Hint h = diagnose(e);
    const Hint old = static_cast<Hint>(InterlockedExchange(&hint_, static_cast<LONG>(h)));
    if (h == old) return;
    if (h != Hint::None)
        note("%s", hintText(h));
    else
        note("every connected pad has a game controller device now");
}

void Watcher::pollXInput(bool first) {
    if (!xinputCaps_) return;
    uint32_t mask = 0;
    for (DWORD u = 0; u < 4; u++) {
        uint8_t caps[64] = {}; // XINPUT_CAPABILITIES (connection only, no input state)
        if (xinputCaps_(u, kXInputFlagGamepad, caps) == ERROR_SUCCESS) mask |= 1u << u;
    }
    if (first) {
        note(mask ? "XInput: pad in slot %s" : "XInput: no pad%s", slotList(mask).c_str());
    } else if (mask != xinputMask_) {
        const uint32_t on = mask & ~xinputMask_, off = xinputMask_ & ~mask;
        if (on) note("XInput: pad connected in slot %s", slotList(on).c_str());
        if (off) note("XInput: pad disconnected in slot %s", slotList(off).c_str());
        schedule_.trigger(GetTickCount()); // list again even if no HID notification came
        changed_ = true;
    }
    // the hint waits for the DirectInput list that the change starts (0.5 s later): a pad shows up in XInput
    // a moment before DirectInput lists it, which is no reason for a warning
    xinputMask_ = mask;
}

bool Watcher::enumerate() {
    if (!di_) return false;
    std::vector<Attached> now;
    const HRESULT hr = static_cast<IDirectInput8W*>(di_)->EnumDevices(DI8DEVCLASS_GAMECTRL, &collect, &now, DIEDFL_ATTACHEDONLY);
    if (FAILED(hr)) {
        note("EnumDevices failed (0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }
    for (auto& a : now) {
        GUID g;
        memcpy(&g, a.instance.b, 16);
        a.xinput = xinputOf(g);
    }
    std::vector<Attached> before;
    AcquireSRWLockExclusive(&lock_);
    const bool first = gen_ == 0;
    const bool setChanged = first || !sameSet(now, list_);
    const bool bump = setChanged || changed_;
    if (bump) {
        before.swap(list_);
        list_ = now;
        gen_++;
    }
    ReleaseSRWLockExclusive(&lock_);
    changed_ = false;
    if (first) {
        note("DirectInput lists %u game controller(s)%s", static_cast<unsigned>(now.size()), now.empty() ? "" : ":");
        for (const auto& a : now) note("  %s", describe(a).c_str());
    } else if (setChanged) {
        const Diff d = diff(before, now);
        for (const auto& a : d.added) note("DirectInput: new %s", describe(a).c_str());
        for (const auto& a : d.gone) note("DirectInput: gone %s", describe(a).c_str());
    }
    countXbox();
    updateHint();
    return bump;
}

bool Watcher::fakeEnumerate() {
    wchar_t buf[1024];
    const DWORD n = GetEnvironmentVariableW(L"HORIZON_TEST_HOTPLUG_DEVICES", buf, 1024);
    std::wstring text = (n && n < 1024) ? std::wstring(buf, n) : std::wstring();
    if (!fakeFirst_ && text == fakeText_) return false;
    fakeFirst_ = false;
    fakeText_ = text;
    std::vector<Attached> now;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L';', pos);
        if (end == std::wstring::npos) end = text.size();
        const std::wstring item = text.substr(pos, end - pos);
        const size_t colon = item.find(L':');
        if (colon != std::wstring::npos) {
            Attached a;
            memset(a.instance.b, 0, 16);
            memset(a.product.b, 0, 16);
            const unsigned id = static_cast<unsigned>(wcstoul(item.c_str(), nullptr, 10));
            memcpy(a.instance.b, &id, sizeof(id));
            a.instance.b[15] = 0x7e; // never a real GUID
            a.product.b[0] = 0x99;
            a.name = item.substr(colon + 1);
            a.devType = DI8DEVTYPE_GAMEPAD;
            now.push_back(a);
        }
        pos = end + 1;
    }
    AcquireSRWLockExclusive(&lock_);
    list_.swap(now);
    gen_++;
    const size_t count = list_.size();
    ReleaseSRWLockExclusive(&lock_);
    note("test devices: %u", static_cast<unsigned>(count));
    return true;
}

void Watcher::run() {
    HWND wnd = nullptr;
    HDEVNOTIFY dn = nullptr;
    wchar_t cls[64] = L"";
    if (!fake_) {
        HMODULE sys = loadSystemDll(L"dinput8.dll"); // stays loaded (dinput8 is in the process anyway)
        auto create = sys ? reinterpret_cast<Create8Fn>(GetProcAddress(sys, "DirectInput8Create")) : nullptr;
        if (!create || FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, &di_, nullptr)) || !di_) {
            di_ = nullptr;
            note("own DirectInput object could not be created: no controller watch");
        }
        swprintf_s(cls, L"HorizonHotplugWatch.%lu.%p", GetCurrentProcessId(), static_cast<void*>(this));
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &wndProc;
        wc.hInstance = selfModule();
        wc.lpszClassName = cls;
        if (di_ && RegisterClassExW(&wc)) {
            wnd = CreateWindowExW(0, cls, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
            if (wnd) {
                SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
                DEV_BROADCAST_DEVICEINTERFACE_W f = {};
                f.dbcc_size = sizeof(f);
                f.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
                f.dbcc_classguid = kHidInterface;
                dn = RegisterDeviceNotificationW(wnd, &f, DEVICE_NOTIFY_WINDOW_HANDLE);
            }
        }
        InterlockedExchange(&notify_, dn ? 1 : 0);
        if (di_) {
            if (dn)
                note("watching HID device notifications");
            else
                note("no device notifications (error %lu): listing every 3 s", GetLastError());
            // XInput: connection state only (XInputGetCapabilities), a second way to see an Xbox-class pad come
            HMODULE xi = loadSystemDll(L"xinput1_4.dll");
            if (!xi) xi = loadSystemDll(L"xinput9_1_0.dll");
            xinputCaps_ = xi ? reinterpret_cast<CapsFn>(GetProcAddress(xi, "XInputGetCapabilities")) : nullptr;
            if (!xinputCaps_) note("XInput not available");
        }
    }

    // first list right away
    bool firstKick = false;
    if (fake_) {
        firstKick = fakeEnumerate();
    } else if (di_) {
        pollXInput(true);
        firstKick = enumerate(); // also counts the Xbox pads and logs why the game cannot see one
    }
    if (firstKick) kick_(user_);
    uint32_t nextPoll = GetTickCount() + (fake_ ? kFakeMs : kPollMs);
    uint32_t nextXInput = GetTickCount() + kXInputMs;
    uint32_t nextTick = GetTickCount() + kTickMs;
    for (;;) {
        const uint32_t now = GetTickCount();
        uint32_t wait = 60000;
        auto consider = [&](bool active, uint32_t at) {
            if (!active) return;
            const uint32_t w = due(now, at) ? 0 : at - now;
            if (w < wait) wait = w;
        };
        const uint32_t sw = schedule_.waitMs(now);
        if (sw < wait) wait = sw;
        const bool polling = fake_ || (di_ && !dn);
        consider(polling, nextPoll);
        consider(xinputCaps_ != nullptr, nextXInput);
        const bool ticks = ticks_ != 0;
        consider(ticks, nextTick);
        const DWORD r = MsgWaitForMultipleObjects(1, &stop_, FALSE, wait, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) break;
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        const uint32_t t = GetTickCount();
        bool kick = false;
        if (xinputCaps_ && due(t, nextXInput)) {
            nextXInput = t + kXInputMs;
            pollXInput(false); // may start a list series
        }
        if (schedule_.take(t) && !fake_ && enumerate()) kick = true;
        if (polling && due(t, nextPoll)) {
            nextPoll = t + (fake_ ? kFakeMs : kPollMs);
            if (fake_ ? fakeEnumerate() : enumerate()) kick = true;
        }
        if (ticks_ && due(t, nextTick)) {
            nextTick = t + kTickMs;
            kick = true;
        } else if (!ticks_) {
            nextTick = t + kTickMs;
        }
        if (kick) kick_(user_);
    }
    if (dn) UnregisterDeviceNotification(dn);
    if (wnd) DestroyWindow(wnd);
    if (cls[0]) UnregisterClassW(cls, selfModule());
    if (di_) static_cast<IDirectInput8W*>(di_)->Release();
    di_ = nullptr;
}

} // namespace hp
