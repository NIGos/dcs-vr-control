#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <cstring>
#include <algorithm>

// A headless test runtime. It has no connection to SteamVR, Pimax, or a headset.
extern "C" {
XrResult XRAPI_CALL create_instance(const XrInstanceCreateInfo*, XrInstance* instance) { *instance = reinterpret_cast<XrInstance>(1); return XR_SUCCESS; }
XrResult XRAPI_CALL destroy_instance(XrInstance) { return XR_SUCCESS; }
XrResult XRAPI_CALL extensions(const char* layer, uint32_t capacity, uint32_t* count, XrExtensionProperties* props) {
    if (layer) return XR_ERROR_API_LAYER_NOT_PRESENT;
    const char* names[]{"XR_KHR_D3D11_enable", "XR_KHR_D3D12_enable", "XR_EXT_eye_gaze_interaction"};
    *count = 3; if (!capacity) return XR_SUCCESS; if (capacity < 3) return XR_ERROR_SIZE_INSUFFICIENT;
    for (unsigned i = 0; i < 3; ++i) { strcpy_s(props[i].extensionName, names[i]); props[i].extensionVersion = 1; }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL instance_properties(XrInstance, XrInstanceProperties* p) { strcpy_s(p->runtimeName, "DCSVR Offline Fixture"); p->runtimeVersion = XR_MAKE_VERSION(1,0,0); return XR_SUCCESS; }
XrResult XRAPI_CALL system_id(XrInstance, const XrSystemGetInfo*, XrSystemId* id) { *id = 1; return XR_SUCCESS; }
XrResult XRAPI_CALL system_properties(XrInstance, XrSystemId, XrSystemProperties* p) {
    p->systemId=1; strcpy_s(p->systemName,"Offline headset fixture"); p->graphicsProperties={4096,4096,16}; p->trackingProperties={XR_TRUE,XR_TRUE};
    return XR_SUCCESS;
}
XrResult XRAPI_CALL configs(XrInstance, XrSystemId, uint32_t capacity, uint32_t* count, XrViewConfigurationType* views) {
    *count=1; if(capacity) views[0]=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; return XR_SUCCESS;
}
XrResult XRAPI_CALL config_properties(XrInstance, XrSystemId, XrViewConfigurationType type, XrViewConfigurationProperties* p) { p->viewConfigurationType=type; p->fovMutable=XR_TRUE; return XR_SUCCESS; }
XrResult XRAPI_CALL config_views(XrInstance, XrSystemId, XrViewConfigurationType type, uint32_t capacity, uint32_t* count, XrViewConfigurationView* views) {
    if(type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    *count=2; if(!capacity) return XR_SUCCESS; if(capacity<2) return XR_ERROR_SIZE_INSUFFICIENT;
    for(unsigned i=0;i<2;++i) { views[i].recommendedImageRectWidth=1024; views[i].recommendedImageRectHeight=1024; views[i].maxImageRectWidth=4096; views[i].maxImageRectHeight=4096; views[i].recommendedSwapchainSampleCount=1; views[i].maxSwapchainSampleCount=1; }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL blend_modes(XrInstance,XrSystemId,XrViewConfigurationType,uint32_t capacity,uint32_t* count,XrEnvironmentBlendMode* modes) { *count=1; if(capacity) modes[0]=XR_ENVIRONMENT_BLEND_MODE_OPAQUE; return XR_SUCCESS; }
// Lookup-only stubs: this fixture never creates a session or submits frames.
XrResult XRAPI_CALL unavailable_session_operation(XrSession) { return XR_ERROR_FUNCTION_UNSUPPORTED; }
XrResult XRAPI_CALL gipa(XrInstance, const char* name, PFN_xrVoidFunction* out) {
    *out=nullptr;
#define ENTRY(n,f) if(strcmp(name,n)==0) { *out=reinterpret_cast<PFN_xrVoidFunction>(f); return XR_SUCCESS; }
    ENTRY("xrGetInstanceProcAddr",gipa) ENTRY("xrCreateInstance",create_instance) ENTRY("xrDestroyInstance",destroy_instance)
    ENTRY("xrEnumerateInstanceExtensionProperties",extensions) ENTRY("xrGetInstanceProperties",instance_properties)
    ENTRY("xrGetSystem",system_id) ENTRY("xrGetSystemProperties",system_properties) ENTRY("xrEnumerateViewConfigurations",configs)
    ENTRY("xrGetViewConfigurationProperties",config_properties) ENTRY("xrEnumerateViewConfigurationViews",config_views) ENTRY("xrEnumerateEnvironmentBlendModes",blend_modes)
    const char* lookups[]{"xrCreateSession","xrDestroySession","xrBeginSession","xrEndSession","xrWaitFrame","xrBeginFrame","xrEndFrame","xrCreateSwapchain","xrDestroySwapchain","xrDestroySpace","xrEnumerateSwapchainImages","xrAcquireSwapchainImage","xrWaitSwapchainImage","xrReleaseSwapchainImage"};
    for(const auto* lookup:lookups) if(strcmp(name,lookup)==0) { *out=reinterpret_cast<PFN_xrVoidFunction>(unavailable_session_operation); return XR_SUCCESS; }
    // Quad Views populates its complete dispatch table at creation. Unused
    // functions resolve to a failure stub; calling one cannot access hardware.
    if(strncmp(name,"xr",2)==0) { *out=reinterpret_cast<PFN_xrVoidFunction>(unavailable_session_operation); return XR_SUCCESS; }
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
__declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* info, XrNegotiateRuntimeRequest* request) {
    if(!info || !request || info->maxInterfaceVersion<1) return XR_ERROR_INITIALIZATION_FAILED;
    request->runtimeInterfaceVersion=1; request->runtimeApiVersion=XR_MAKE_VERSION(1,0,26); request->getInstanceProcAddr=gipa; return XR_SUCCESS;
}
}
