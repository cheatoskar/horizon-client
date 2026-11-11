// Test stand-in for an earlier dinput8.dll (renamed to dinput8.chain.dll): writes chain_called.txt next
// to itself on DirectInput8Create and forwards to the system dinput8.dll.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <unknwn.h>

static HMODULE g_self = nullptr;

extern "C" HRESULT WINAPI Chain_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, IUnknown* outer) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_self, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) {
        wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"chain_called.txt");
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    wcscat_s(sys, L"\\dinput8.dll");
    HMODULE real = LoadLibraryW(sys);
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);
    Fn fn = real ? reinterpret_cast<Fn>(GetProcAddress(real, "DirectInput8Create")) : nullptr;
    return fn ? fn(inst, version, riid, out, outer) : E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) g_self = inst;
    return TRUE;
}
