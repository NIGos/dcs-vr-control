// Verifies with the real OpenXR loader how an implicit gaze bridge is ordered relative to profile layers.
// The machine's registry is never read or changed: HKLM and HKCU are redirected for this process only to a
// private, empty test key that is deleted afterwards.
#include <windows.h>
#include <openxr/openxr.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }

HKEY create(HKEY parent, const std::wstring& path) {
    HKEY key{};
    require(RegCreateKeyExW(parent, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS, "Cannot create test key");
    return key;
}

std::string order_for_instance() {
    SetEnvironmentVariableA("DCSVR_PROBE_ORDER", "");
    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(create.applicationInfo.applicationName, "DCSVR gaze order test");
    create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    XrInstance instance{};
    const auto result = xrCreateInstance(&create, &instance);
    require(result == XR_SUCCESS, "xrCreateInstance failed: " + std::to_string(result));
    xrDestroyInstance(instance);
    char order[1024]{};
    GetEnvironmentVariableA("DCSVR_PROBE_ORDER", order, sizeof(order));
    return order;
}
}

// Usage: gaze_order <implicit-manifest> <expected-order>
int main(int argc, char** argv) {
    const std::wstring root = L"Software\\DcsVrControlTests\\gaze-order-" + std::to_wstring(GetCurrentProcessId());
    int code = 1;
    try {
        require(argc == 3, "Usage: gaze_order <implicit-manifest> <expected-order>");
        const std::wstring manifest(argv[1], argv[1] + std::strlen(argv[1]));
        HKEY machine = create(HKEY_CURRENT_USER, root + L"\\machine");
        HKEY user = create(HKEY_CURRENT_USER, root + L"\\user");
        HKEY layers = create(user, L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit");
        const DWORD enabled = 0;
        require(RegSetValueExW(layers, manifest.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&enabled), sizeof(enabled)) == ERROR_SUCCESS, "Cannot register test layer");
        RegCloseKey(layers);
        require(RegOverridePredefKey(HKEY_LOCAL_MACHINE, machine) == ERROR_SUCCESS, "HKLM redirection failed");
        require(RegOverridePredefKey(HKEY_CURRENT_USER, user) == ERROR_SUCCESS, "HKCU redirection failed");
        const auto order = order_for_instance();
        RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr); RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        RegCloseKey(machine); RegCloseKey(user);
        std::cout << "ORDER " << order << '\n';
        require(order == argv[2], std::string("Expected ") + argv[2]);
        std::cout << "PASS real OpenXR loader layer order " << order << '\n';
        code = 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; }
    RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr); RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\DcsVrControlTests");
    return code;
}
