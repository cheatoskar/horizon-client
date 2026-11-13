// Stand-in for Competition Patch in tests\host_test.cpp (built as build\test\cp\TMCompPatch.dll): does nothing,
// only its module name matters. Loaded after the host started, the host must stop touching the input.
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
