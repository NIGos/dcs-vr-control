// Read-only query: does the active (or XR_RUNTIME_JSON) OpenXR runtime report eye-gaze interaction support?
// Creates an instance and asks for system properties; no session, no frames, no configuration change.
#include <windows.h>
#include <openxr/openxr.h>
#include <cstring>
#include <iostream>

int main() {
    const char* extensions[]{XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME};
    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(create.applicationInfo.applicationName, "DCSVR gaze probe");
    create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    create.enabledExtensionCount = 1;
    create.enabledExtensionNames = extensions;
    XrInstance instance{};
    XrResult result = xrCreateInstance(&create, &instance);
    if (XR_FAILED(result)) { std::cout << "CREATE_FAILED " << result << '\n'; return 1; }
    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(instance, &properties);
    std::cout << "RUNTIME " << properties.runtimeName << '\n';
    XrSystemGetInfo get{XR_TYPE_SYSTEM_GET_INFO};
    get.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system{};
    result = xrGetSystem(instance, &get, &system);
    if (XR_FAILED(result)) { std::cout << "NO_SYSTEM " << result << '\n'; xrDestroyInstance(instance); return 2; }
    XrSystemEyeGazeInteractionPropertiesEXT gaze{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
    XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES, &gaze};
    xrGetSystemProperties(instance, system, &system_properties);
    std::cout << "SYSTEM " << system_properties.systemName << '\n';
    std::cout << "EYE_GAZE_SUPPORTED " << (gaze.supportsEyeGazeInteraction ? 1 : 0) << '\n';
    xrDestroyInstance(instance);
    return 0;
}
