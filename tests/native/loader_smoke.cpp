#include <windows.h>
#include <openxr/openxr.h>
#include <iostream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <dbghelp.h>

LONG CALLBACK fault_trace(EXCEPTION_POINTERS* fault) {
    if (fault->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    auto context = *fault->ContextRecord;
    STACKFRAME64 stack{};
    stack.AddrPC = {context.Rip, AddrModeFlat}; stack.AddrStack = {context.Rsp, AddrModeFlat}; stack.AddrFrame = {context.Rbp, AddrModeFlat};
    std::cerr << "ACCESS VIOLATION trace\n";
    for (unsigned i = 0; i < 16; ++i) {
        HMODULE module{}; wchar_t path[MAX_PATH]{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(stack.AddrPC.Offset), &module);
        GetModuleFileNameW(module, path, MAX_PATH);
        std::wcerr << path << L" + 0x" << std::hex << (stack.AddrPC.Offset - reinterpret_cast<ULONG64>(module)) << L'\n';
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &stack,
                &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
int main(int argc,char** argv) {
    AddVectoredExceptionHandler(1, fault_trace);
    try {
        require(argc==2,"Expected quad or stereo mode"); const bool quad=strcmp(argv[1],"quad")==0;
        uint32_t count{}; require(xrEnumerateApiLayerProperties(0,&count,nullptr)==XR_SUCCESS,"Enumerate layer count failed");
        std::vector<XrApiLayerProperties> layers(count,{XR_TYPE_API_LAYER_PROPERTIES});
        require(xrEnumerateApiLayerProperties(count,&count,layers.data())==XR_SUCCESS,"Enumerate layers failed");
        for(const auto& layer:layers) std::cout<<"LAYER "<<layer.layerName<<'\n';
        require(count>0,"Explicit layers were not discovered");
        uint32_t extCount{}; require(xrEnumerateInstanceExtensionProperties(nullptr,0,&extCount,nullptr)==XR_SUCCESS,"Enumerate extensions failed");
        std::vector<XrExtensionProperties> extensions(extCount,{XR_TYPE_EXTENSION_PROPERTIES});
        require(xrEnumerateInstanceExtensionProperties(nullptr,extCount,&extCount,extensions.data())==XR_SUCCESS,"Extensions failed");
        if(quad) require(std::any_of(extensions.begin(),extensions.end(),[](const auto& e){return strcmp(e.extensionName,"XR_VARJO_quad_views")==0;}),"Quad extension not advertised");
        const char* enabled[]{"XR_KHR_D3D11_enable","XR_VARJO_quad_views"}; XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(create.applicationInfo.applicationName,"DCSVR Offline Test"); create.applicationInfo.apiVersion=XR_MAKE_VERSION(1,0,0);
        create.enabledExtensionCount=quad?2:1; create.enabledExtensionNames=enabled;
        XrInstance instance{}; const auto result=xrCreateInstance(&create,&instance); std::cout<<"CREATE "<<result<<'\n'; require(result==XR_SUCCESS,"Actual layer chain failed to create instance");
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES}; require(xrGetInstanceProperties(instance,&properties)==XR_SUCCESS,"Instance properties failed");
        require(strcmp(properties.runtimeName,"DCSVR Offline Fixture")==0,"Unexpected runtime: hardware access prohibited");
        XrSystemGetInfo get{XR_TYPE_SYSTEM_GET_INFO}; get.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY; XrSystemId system{};
        require(xrGetSystem(instance,&get,&system)==XR_SUCCESS,"GetSystem failed");
        uint32_t viewCount{}; require(xrEnumerateViewConfigurationViews(instance,system,quad?XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO:XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,0,&viewCount,nullptr)==XR_SUCCESS,"View enumeration failed");
        require(viewCount==(quad?4U:2U),"Unexpected number of views");
        require(xrDestroyInstance(instance)==XR_SUCCESS,"Instance destruction failed");
        std::cout<<"PASS real OpenXR loader, production layers, fixture runtime, "<<viewCount<<" views\n"; return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n'; return 1; }
}
