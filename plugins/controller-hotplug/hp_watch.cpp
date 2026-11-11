#define DIRECTINPUT_VERSION 0x0800
#include "hp_watch.h"

#include <dbt.h>
#include <dinput.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

namespace hp {

namespace {
// GUID_DEVINTERFACE_HID {4D1E55B2-F16F-11CF-88CB-001111000030}
const GUID kHidInterface = {0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
constexpr uint32_t kFirstEnumMs = 500;
constexpr uint32_t kSecondEnumMs = 2500;
constexpr uint32_t kPollMs = 1000;
constexpr uint32_t kTickMs = 1000;
constexpr uint32_t kFakeMs = 100;

bool due(uint32_t now, uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }

using Create8Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

BOOL CALLBACK collect(LPCDIDEVICEINSTANCEW inst, LPVOID ref) {
    auto* out = static_cast<std::vector<Attached>*>(ref);
    Attached a;
    memcpy(a.instance.b, &inst->guidInstance, 16);
    memcpy(a.product.b, &inst->guidProduct, 16);
    a.name = inst->tszProductName[0] ? inst->tszProductName : inst->tszInstanceName;
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
        if (w) w->onDeviceChange();
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
} // namespace

void Watcher::note(const char* fmt, ...) {
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    AcquireSRWLockExclusive(&lock_);
    if (notes_.size() < 64) notes_.push_back(buf);
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

void Watcher::onDeviceChange() {
    const uint32_t now = GetTickCount();
    enumAt_[0] = now + kFirstEnumMs;
    enumAt_[1] = now + kSecondEnumMs;
    enumPending_[0] = enumPending_[1] = true;
}

bool Watcher::enumerate(bool fromNotification) {
    if (!di_) return false;
    std::vector<Attached> now;
    const HRESULT hr = static_cast<IDirectInput8W*>(di_)->EnumDevices(DI8DEVCLASS_GAMECTRL, &collect, &now, DIEDFL_ATTACHEDONLY);
    if (FAILED(hr)) {
        note("EnumDevices failed (0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }
    AcquireSRWLockExclusive(&lock_);
    const bool changed = gen_ == 0 || !sameSet(now, list_);
    const bool bump = changed || fromNotification;
    if (bump) {
        list_.swap(now);
        gen_++;
    }
    const size_t count = list_.size();
    ReleaseSRWLockExclusive(&lock_);
    if (changed) note("controllers attached: %u", static_cast<unsigned>(count));
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
            now.push_back(a);
        }
        pos = end + 1;
    }
    AcquireSRWLockExclusive(&lock_);
    list_.swap(now);
    gen_++;
    ReleaseSRWLockExclusive(&lock_);
    return true;
}

void Watcher::run() {
    HWND wnd = nullptr;
    HDEVNOTIFY dn = nullptr;
    HMODULE sys = nullptr;
    wchar_t cls[64] = L"";
    if (!fake_) {
        wchar_t path[MAX_PATH];
        const UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n && n < MAX_PATH - 16) {
            wcscat_s(path, L"\\dinput8.dll");
            sys = LoadLibraryW(path);
        }
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
        if (di_) note(dn ? "watching HID device notifications" : "no device notifications: enumerating once per second");
    }

    // first list right away
    if (fake_ ? fakeEnumerate() : enumerate(false)) kick_(user_);
    uint32_t nextPoll = GetTickCount() + (fake_ ? kFakeMs : kPollMs);
    uint32_t nextTick = GetTickCount() + kTickMs;
    for (;;) {
        const uint32_t now = GetTickCount();
        uint32_t wait = 60000;
        auto consider = [&](bool active, uint32_t at) {
            if (!active) return;
            const uint32_t w = due(now, at) ? 0 : at - now;
            if (w < wait) wait = w;
        };
        consider(enumPending_[0], enumAt_[0]);
        consider(enumPending_[1], enumAt_[1]);
        const bool polling = fake_ || (di_ && !dn);
        consider(polling, nextPoll);
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
        for (int i = 0; i < 2; i++)
            if (enumPending_[i] && due(t, enumAt_[i])) {
                enumPending_[i] = false;
                if (enumerate(true)) kick = true;
            }
        if (polling && due(t, nextPoll)) {
            nextPoll = t + (fake_ ? kFakeMs : kPollMs);
            if (fake_ ? fakeEnumerate() : enumerate(false)) kick = true;
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
    // sys stays loaded (dinput8 is in the process anyway)
}

} // namespace hp
