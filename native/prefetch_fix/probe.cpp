// Test stand-in for edCore: keeps PrefetchVirtualMemory in a data pointer resolved at load, and resolves it again
// through its own GetProcAddress import on demand.
#include <windows.h>

using PrefetchFn = BOOL(WINAPI*)(HANDLE, ULONG_PTR, PWIN32_MEMORY_RANGE_ENTRY, ULONG);
static PrefetchFn g_stored = reinterpret_cast<PrefetchFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "PrefetchVirtualMemory"));

extern "C" __declspec(dllexport) BOOL probe_stored(void* address, SIZE_T bytes) {
    WIN32_MEMORY_RANGE_ENTRY range{address, bytes};
    return g_stored(GetCurrentProcess(), 1, &range, 0);
}
extern "C" __declspec(dllexport) BOOL probe_lookup(void* address, SIZE_T bytes) {
    auto fn = reinterpret_cast<PrefetchFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "PrefetchVirtualMemory"));
    WIN32_MEMORY_RANGE_ENTRY range{address, bytes};
    return fn(GetCurrentProcess(), 1, &range, 0);
}
extern "C" __declspec(dllexport) void* probe_stored_target() { return reinterpret_cast<void*>(g_stored); }
