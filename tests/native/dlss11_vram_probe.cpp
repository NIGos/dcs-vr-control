// Measures the VRAM of one DX11 DLSS SR feature exactly as DCS creates it for a
// Quad Views view (DCS Control "Feature ledger" / deferred DX11 feature).
// Offscreen: NVIDIA hardware D3D11 device, the installed NGX core, the game's
// DLSS snippet directory as feature path. Process-local VRAM (QueryVideoMemoryInfo
// CurrentUsage) is sampled around create, first evaluate and release.
// Usage: dlss11_vram_probe <_nvngx.dll> <DLSS feature directory> [renderW renderH outW outH]...
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
// nvsdk_ngx_params.h declares overloaded Set/Get; MSVC lays out overloaded
// virtuals differently from distinct names, so mirror the declaration exactly.
struct NgxParameterTable {
    virtual void Set(const char*, unsigned long long) = 0; virtual void Set(const char*, float) = 0;
    virtual void Set(const char*, double) = 0; virtual void Set(const char*, unsigned int) = 0;
    virtual void Set(const char*, int) = 0; virtual void Set(const char*, ID3D11Resource*) = 0;
    virtual void Set(const char*, struct ID3D12Resource*) = 0; virtual void Set(const char*, void*) = 0;
    void SetULL(const char* n, unsigned long long v) { Set(n, v); } void SetF(const char* n, float v) { Set(n, v); }
    void SetUI(const char* n, unsigned int v) { Set(n, v); } void SetI(const char* n, int v) { Set(n, v); }
    void SetD3d11Resource(const char* n, ID3D11Resource* v) { Set(n, v); }
};
struct PathList { const wchar_t* const* paths; unsigned count; };
struct LoggingInfo { void* callback; int level; bool disable_sinks; };
struct FeatureCommonInfo { PathList paths; void* internal; LoggingInfo logging; };
using Init = unsigned (*)(unsigned long long, const wchar_t*, ID3D11Device*, unsigned, const FeatureCommonInfo*); // Init_Ext
using Allocate = unsigned (*)(NgxParameterTable**);
using Destroy = unsigned (*)(NgxParameterTable*);
using Create = unsigned (*)(ID3D11DeviceContext*, unsigned, NgxParameterTable*, void**);
using Evaluate = unsigned (*)(ID3D11DeviceContext*, const void*, const NgxParameterTable*, void*);
using Release = unsigned (*)(void*);
using Shutdown1 = unsigned (*)(ID3D11Device*);
void ngx_log(const char* message, int, int) { if (message && (std::getenv("DCSVR_NGX_VERBOSE") || std::strstr(message, "rror"))) fputs(message, stdout); }
bool ok(unsigned r) { return (r & 0xFFF00000U) != 0xBAD00000U; }
void require(bool v, const char* s) { if (!v) throw std::runtime_error(s); }

struct Probe {
    ComPtr<IDXGIAdapter3> adapter; ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    double mb() {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{}; adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
        return double(info.CurrentUsage) / (1024.0 * 1024.0);
    }
    void idle() {
        ComPtr<ID3D11Query> q; D3D11_QUERY_DESC d{D3D11_QUERY_EVENT, 0}; device->CreateQuery(&d, &q);
        context->End(q.Get()); context->Flush();
        while (context->GetData(q.Get(), nullptr, 0, 0) == S_FALSE) Sleep(1);
        Sleep(200); // let the driver settle residency accounting
    }
    ComPtr<ID3D11Texture2D> texture(UINT w, UINT h, DXGI_FORMAT f, UINT bind) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1; d.Format = f; d.BindFlags = bind;
        ComPtr<ID3D11Texture2D> t; require(SUCCEEDED(device->CreateTexture2D(&d, nullptr, &t)), "texture"); return t;
    }
};
}

int wmain(int argc, wchar_t** argv) {
    try {
        require(argc >= 3, "Usage: dlss11_vram_probe <_nvngx.dll> <feature directory> [renderW renderH outW outH]...");
        std::vector<std::array<UINT, 4>> cases;
        for (int i = 3; i + 3 < argc; i += 4) cases.push_back({UINT(_wtoi(argv[i])), UINT(_wtoi(argv[i + 1])), UINT(_wtoi(argv[i + 2])), UINT(_wtoi(argv[i + 3]))});
        if (cases.empty()) cases = {{1204, 1188, 2076, 2048}, {604, 596, 1040, 1028}};
        Probe p;
        ComPtr<IDXGIFactory6> factory; require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "factory");
        for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&p.adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{}; p.adapter->GetDesc1(&d);
            if (d.VendorId == 0x10DE && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { wprintf(L"Hardware: %s\n", d.Description); break; }
            p.adapter.Reset();
        }
        require(p.adapter != nullptr, "No NVIDIA adapter");
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
        require(SUCCEEDED(D3D11CreateDevice(p.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &p.device, nullptr, &p.context)), "device");
        const auto core = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        require(core != nullptr, "core");
        const auto init = reinterpret_cast<Init>(GetProcAddress(core, "NVSDK_NGX_D3D11_Init_Ext"));
        const auto allocate = reinterpret_cast<Allocate>(GetProcAddress(core, "NVSDK_NGX_D3D11_AllocateParameters"));
        const auto destroy = reinterpret_cast<Destroy>(GetProcAddress(core, "NVSDK_NGX_D3D11_DestroyParameters"));
        const auto create = reinterpret_cast<Create>(GetProcAddress(core, "NVSDK_NGX_D3D11_CreateFeature"));
        const auto evaluate = reinterpret_cast<Evaluate>(GetProcAddress(core, "NVSDK_NGX_D3D11_EvaluateFeature"));
        const auto release = reinterpret_cast<Release>(GetProcAddress(core, "NVSDK_NGX_D3D11_ReleaseFeature"));
        const auto shutdown = reinterpret_cast<Shutdown1>(GetProcAddress(core, "NVSDK_NGX_D3D11_Shutdown1"));
        require(init && allocate && destroy && create && evaluate && release && shutdown, "NGX D3D11 exports");
        wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
        const wchar_t* paths[]{argv[2]};
        FeatureCommonInfo info{{paths, 1}, nullptr, {reinterpret_cast<void*>(&ngx_log), 2, false}};
        FeatureCommonInfo no_path{{nullptr, 0}, nullptr, {reinterpret_cast<void*>(&ngx_log), 2, false}};
        unsigned flags = 0x2BU;
        if (const char* text = std::getenv("DCSVR_DLSS_FLAGS")) flags = unsigned(std::strtoul(text, nullptr, 0));
        struct Kept { void* handle; NgxParameterTable* params; ComPtr<ID3D11Texture2D> c, d, m, o; };
        std::vector<Kept> kept;
        const bool keep = std::getenv("DCSVR_PROBE_KEEP") != nullptr;
        const auto measure = [&]() -> bool {
            for (const auto& c : cases) {
                NgxParameterTable* params{}; require(ok(allocate(&params)) && params, "AllocateParameters");
                params->SetUI("Width", c[0]); params->SetUI("Height", c[1]); params->SetUI("OutWidth", c[2]); params->SetUI("OutHeight", c[3]);
                params->SetI("PerfQualityValue", 1); params->SetI("DLSS.Feature.Create.Flags", int(flags));
                params->SetUI("CreationNodeMask", 1); params->SetUI("VisibilityNodeMask", 1); params->SetI("DLSS.Enable.Output.Subrects", 0);
                // NVSDK_NGX_Parameter_FreeMemOnReleaseFeature: release returns the feature's VRAM instead of pooling it.
                if (std::getenv("DCSVR_PROBE_FREE_ON_RELEASE")) params->SetI("FreeMemOnReleaseFeature", 1);
                auto color = p.texture(c[0], c[1], DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
                auto depth = p.texture(c[0], c[1], DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
                auto motion = p.texture(c[0], c[1], DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
                auto output = p.texture(c[2], c[3], DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
                p.idle(); const double before = p.mb();
                void* handle{}; const auto started = GetTickCount64();
                const auto created = create(p.context.Get(), 1, params, &handle);
                const auto create_ms = GetTickCount64() - started;
                if (!ok(created) || !handle) { printf("CreateFeature result=0x%08X\n", created); destroy(params); return false; }
                p.idle(); const double after_create = p.mb();
                params->SetD3d11Resource("Color", color.Get()); params->SetD3d11Resource("Depth", depth.Get());
                params->SetD3d11Resource("MotionVectors", motion.Get()); params->SetD3d11Resource("Output", output.Get());
                params->SetF("MV.Scale.X", 1.f); params->SetF("MV.Scale.Y", 1.f); params->SetF("Jitter.Offset.X", 0.f); params->SetF("Jitter.Offset.Y", 0.f);
                D3D11_TEXTURE2D_DESC ed{}; ed.Width = ed.Height = ed.MipLevels = ed.ArraySize = ed.SampleDesc.Count = 1; ed.Format = DXGI_FORMAT_R32_FLOAT; ed.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                const float one = 1.f; D3D11_SUBRESOURCE_DATA ei{&one, 4, 4}; ComPtr<ID3D11Texture2D> exposure;
                require(SUCCEEDED(p.device->CreateTexture2D(&ed, &ei, &exposure)), "exposure");
                params->SetD3d11Resource("ExposureTexture", exposure.Get()); params->SetF("Sharpness", 0.f);
                params->SetF("DLSS.Pre.Exposure", 1.f); params->SetF("DLSS.Exposure.Scale", 1.f);
                params->SetI("Reset", 1); params->SetUI("DLSS.Render.Subrect.Dimensions.Width", c[0]); params->SetUI("DLSS.Render.Subrect.Dimensions.Height", c[1]);
                for (unsigned frame = 0; frame < 3; ++frame) {
                    const auto evaluated = evaluate(p.context.Get(), handle, params, nullptr);
                    if (!ok(evaluated)) printf("EvaluateFeature result=0x%08X\n", evaluated);
                    params->SetI("Reset", 0);
                }
                p.idle(); const double after_evaluate = p.mb();
                // DCSVR_PROBE_KEEP=1 keeps every feature alive (like DCS's four views) and
                // releases them together at the end, so per-feature costs are not pooled.
                if (keep) kept.push_back({handle, params, color, depth, motion, output});
                else { require(ok(release(handle)), "ReleaseFeature"); destroy(params); }
                p.idle(); const double after_release = p.mb();
                printf("DX11 DLSS SR feature %ux%u -> %ux%u flags=0x%X quality=1: create %llu ms, VRAM create %+.1f MiB, "
                    "after 3 evaluations %+.1f MiB, release %+.1f MiB (process local usage %.1f -> %.1f -> %.1f -> %.1f MiB)\n",
                    c[0], c[1], c[2], c[3], flags, static_cast<unsigned long long>(create_ms), after_create - before, after_evaluate - before,
                    after_release - after_evaluate, before, after_create, after_evaluate, after_release);
            }
            if (keep && !kept.empty()) {
                // Last created first: what one release returns while the others stay alive.
                for (auto k = kept.rbegin(); k != kept.rend(); ++k) {
                    const double before_release = p.mb();
                    require(ok(release(k->handle)), "ReleaseFeature"); destroy(k->params);
                    p.idle(); const double immediate = p.mb(); Sleep(2000); p.idle();
                    const double waited = p.mb();
                    // The game keeps evaluating its other features (DCS: the peripheral views).
                    for (unsigned frame = 0; frame < 30; ++frame)
                        for (auto other = kept.begin(); other != (k + 1).base(); ++other)
                            static_cast<void>(evaluate(p.context.Get(), other->handle, other->params, nullptr));
                    p.idle(); Sleep(1000); p.idle();
                    const auto desc = [&] { D3D11_TEXTURE2D_DESC d{}; k->o->GetDesc(&d); return d; }();
                    printf("Release of kept %ux%u-output feature: process local usage %.1f -> %.1f (immediate %+.1f MiB) -> %.1f after 2 s -> %.1f MiB after 30 frames of the remaining features (%+.1f MiB)\n",
                        desc.Width, desc.Height, before_release, immediate, immediate - before_release, waited, p.mb(), p.mb() - before_release);
                }
                kept.clear();
                Sleep(10000); p.idle();
                printf("10 s after the releases: %.1f MiB\n", p.mb());
                p.context->ClearState(); p.context->Flush();
                ComPtr<IDXGIDevice3> dxgi3; if (SUCCEEDED(p.device.As(&dxgi3))) dxgi3->Trim();
                p.idle(); printf("After ClearState+Trim: %.1f MiB\n", p.mb());
            }
            return true;
        };
        bool measured{};
        // DCS's NGX application id and SDK version (as recovered in its Cheeky log), with and
        // without its DLSS directory; NVIDIA's sample application id if the core refuses those.
        for (unsigned long long app : {241534723ULL, 231313132ULL}) {
            for (const auto* feature_info : {&info, &no_path}) {
                const auto initialized = init(app, temp, p.device.Get(), 0x13U, feature_info);
                printf("NVSDK_NGX_D3D11_Init app=%llu featurePath=%s result=0x%08X\n", app, feature_info == &info ? "game" : "none", initialized);
                if (ok(initialized)) measured = measure();
                shutdown(p.device.Get());
                p.idle(); Sleep(1000); p.idle();
                printf("After NGX D3D11 Shutdown1: %.1f MiB\n", p.mb());
                if (measured) break;
            }
            if (measured) break;
        }
        require(measured, "No NGX session could create the DX11 DLSS feature");
        puts("PASS: DX11 DLSS feature VRAM measured");
        return 0;
    } catch (const std::exception& e) { fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
