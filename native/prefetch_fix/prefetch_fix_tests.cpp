// Runs each case in a child process, because the fix reads its mode once when it loads.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

using Probe = BOOL (*)(void*, SIZE_T);
using Stats = unsigned long long (*)(unsigned);

static int fail(const char* what) { std::printf("FAIL: %s\n", what); return 1; }

static int run_case(const char* mode) {
    HMODULE probe = LoadLibraryW(L"prefetch_probe.dll");
    HMODULE fix = LoadLibraryW(L"prefetch_fix.dll");
    if (!probe || !fix) return fail("load");
    auto stored = reinterpret_cast<Probe>(GetProcAddress(probe, "probe_stored"));
    auto lookup = reinterpret_cast<Probe>(GetProcAddress(probe, "probe_lookup"));
    auto stats = reinterpret_cast<Stats>(GetProcAddress(fix, "DcsVrPrefetchFix_Stats"));
    Sleep(1500);  // first module scan
    static char buffer[1 << 20];
    if (!stored(buffer, sizeof(buffer)) || !stored(buffer, sizeof(buffer))) return fail("stored call failed");
    if (!lookup(buffer + 4096, 4096) || !lookup(buffer + 4096, 4096)) return fail("lookup call failed");
    const auto calls = stats(0), skipped = stats(1), pointers = stats(2), imports = stats(3);
    std::printf("%s: calls=%llu skipped=%llu pointers=%llu imports=%llu\n", mode, calls, skipped, pointers, imports);
    if (std::strcmp(mode, "off") == 0) return calls == 0 && pointers == 0 && imports == 0 ? 0 : fail("off mode touched the process");
    if (pointers < 1 || imports < 1) return fail("stored pointer or GetProcAddress import not redirected");
    if (calls != 4) return fail("not every call went through the fix");
    if (std::strcmp(mode, "observe") == 0) return skipped == 0 ? 0 : fail("observe skipped a call");
    if (skipped != 2) return fail("skip mode did not drop exactly the two repeats");
    Sleep(1100);  // outside the window the same range passes again
    stored(buffer, sizeof(buffer));
    return stats(1) == 2 ? 0 : fail("repeat outside the window was skipped");
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--case") == 0) return run_case(argv[2]);
    char self[MAX_PATH]{};
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    int failures = 0;
    // The settings file next to the DLL (a DCS start without DCS Control) and the launch environment overriding it.
    std::string ini = self;
    ini = ini.substr(0, ini.find_last_of('\\') + 1) + "DcsControlPrefetchFix.ini";
    struct Case { const char* env; const char* expect; bool file; };
    const Case cases[] = {{"off", "off", false}, {"observe", "observe", false}, {"skip", "skip", false},
                          {nullptr, "skip", true}, {"off", "off", true}};
    for (const auto& c : cases) {
        const char* mode = c.expect;
        SetEnvironmentVariableA("DCSVR_PREFETCH_MODE", c.env);
        if (c.file) WritePrivateProfileStringA("PrefetchFix", "Mode", "skip", ini.c_str()); else DeleteFileA(ini.c_str());
        std::string command = std::string("\"") + self + "\" --case " + mode;
        STARTUPINFOA si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return fail("spawn");
        WaitForSingleObject(pi.hProcess, 30000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        std::printf("case %s%s: %s\n", mode, c.file ? (c.env ? " (settings file, environment overrides)" : " (settings file only)") : "", code == 0 ? "PASS" : "FAIL");
        failures += code != 0;
    }
    DeleteFileA(ini.c_str());
    std::printf(failures ? "FAILED\n" : "PASS: prefetch fix\n");
    return failures ? 1 : 0;
}
