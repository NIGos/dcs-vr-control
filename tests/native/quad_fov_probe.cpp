// Live probe: which field of view does the runtime give the focus views of XR_VARJO_quad_views?
// Creates a D3D11 session with the PRIMARY_QUAD_VARJO configuration, runs an empty frame loop for a few
// seconds and prints xrLocateViews results with and without XrViewLocateFoveatedRenderingVARJO.
// Read-only towards the runtime configuration: no settings or files are changed.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static FILE* g_log = nullptr;
static void out(const char* fmt, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    fputs(buffer, stdout);
    fflush(stdout);
    if (g_log) { fputs(buffer, g_log); fflush(g_log); }
}

static constexpr double kDeg = 57.29577951308232;

#define CHECK(call)                                                                 \
    do {                                                                            \
        XrResult r_ = (call);                                                       \
        if (XR_FAILED(r_)) { out("FAILED %s -> %d\n", #call, (int)r_); return false; } \
    } while (0)

struct Probe {
    XrInstance instance{};
    XrSystemId system{};
    XrSession session{};
    XrSpace local{}, view{}, gazeSpace{};
    XrActionSet actionSet{};
    XrAction gazeAction{};
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    bool hasFoveated = false, hasGaze = false, running = false, quit = false;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    std::vector<XrViewConfigurationView> plainViews;

    bool HasExt(const std::vector<XrExtensionProperties>& list, const char* name) {
        for (auto& e : list) if (strcmp(e.extensionName, name) == 0) return true;
        return false;
    }

    bool Create() {
        uint32_t count = 0;
        CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr));
        std::vector<XrExtensionProperties> exts(count, {XR_TYPE_EXTENSION_PROPERTIES});
        CHECK(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, exts.data()));
        out("Runtime offers %u extensions; relevant:\n", count);
        for (auto& e : exts) {
            if (strstr(e.extensionName, "VARJO") || strstr(e.extensionName, "gaze") || strstr(e.extensionName, "D3D11") ||
                strstr(e.extensionName, "foveat"))
                out("  %s v%u\n", e.extensionName, e.extensionVersion);
        }
        std::vector<const char*> enable{XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_VARJO_QUAD_VIEWS_EXTENSION_NAME};
        if (!HasExt(exts, XR_VARJO_QUAD_VIEWS_EXTENSION_NAME)) { out("XR_VARJO_quad_views NOT offered\n"); return false; }
        hasFoveated = HasExt(exts, XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME);
        hasGaze = HasExt(exts, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
        if (hasFoveated) enable.push_back(XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME);
        if (hasGaze) enable.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
        XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(ci.applicationInfo.applicationName, "DCSVR quad fov probe");
        ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        ci.enabledExtensionCount = (uint32_t)enable.size();
        ci.enabledExtensionNames = enable.data();
        CHECK(xrCreateInstance(&ci, &instance));
        XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
        xrGetInstanceProperties(instance, &ip);
        out("Runtime: %s %u.%u.%u\n", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
            XR_VERSION_PATCH(ip.runtimeVersion));
        out("Enabled: foveated_rendering=%d eye_gaze=%d\n", hasFoveated, hasGaze);
        XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
        gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        CHECK(xrGetSystem(instance, &gi, &system));

        XrSystemFoveatedRenderingPropertiesVARJO fov{XR_TYPE_SYSTEM_FOVEATED_RENDERING_PROPERTIES_VARJO};
        XrSystemEyeGazeInteractionPropertiesEXT gaze{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
        XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
        void* chain = nullptr;
        if (hasFoveated) { fov.next = chain; chain = &fov; }
        if (hasGaze) { gaze.next = chain; chain = &gaze; }
        sp.next = chain;
        CHECK(xrGetSystemProperties(instance, system, &sp));
        out("System: %s vendor=%u maxSwapchain=%ux%u layers=%u\n", sp.systemName, sp.vendorId, sp.graphicsProperties.maxSwapchainImageWidth,
            sp.graphicsProperties.maxSwapchainImageHeight, sp.graphicsProperties.maxLayerCount);
        if (hasFoveated) out("XrSystemFoveatedRenderingPropertiesVARJO.supportsFoveatedRendering = %u\n", fov.supportsFoveatedRendering);
        if (hasGaze) out("XrSystemEyeGazeInteractionPropertiesEXT.supportsEyeGazeInteraction = %u\n", gaze.supportsEyeGazeInteraction);
        return true;
    }

    bool PrintViewConfigs() {
        uint32_t count = 0;
        CHECK(xrEnumerateViewConfigurations(instance, system, 0, &count, nullptr));
        std::vector<XrViewConfigurationType> types(count);
        CHECK(xrEnumerateViewConfigurations(instance, system, count, &count, types.data()));
        out("View configurations:");
        for (auto t : types) out(" %d", (int)t);
        out("\n");
        for (auto type : {XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO}) {
            uint32_t n = 0;
            if (XR_FAILED(xrEnumerateViewConfigurationViews(instance, system, type, 0, &n, nullptr))) continue;
            std::vector<XrViewConfigurationView> v(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
            CHECK(xrEnumerateViewConfigurationViews(instance, system, type, n, &n, v.data()));
            out("Config %d recommended (plain):\n", (int)type);
            for (uint32_t i = 0; i < n; ++i)
                out("  view %u: rec %ux%u max %ux%u samples %u\n", i, v[i].recommendedImageRectWidth, v[i].recommendedImageRectHeight,
                    v[i].maxImageRectWidth, v[i].maxImageRectHeight, v[i].recommendedSwapchainSampleCount);
            if (type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) plainViews = v;
            if (type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO && hasFoveated) {
                std::vector<XrFoveatedViewConfigurationViewVARJO> f(n, {XR_TYPE_FOVEATED_VIEW_CONFIGURATION_VIEW_VARJO});
                std::vector<XrViewConfigurationView> v2(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
                for (uint32_t i = 0; i < n; ++i) { f[i].foveatedRenderingActive = XR_TRUE; v2[i].next = &f[i]; }
                CHECK(xrEnumerateViewConfigurationViews(instance, system, type, n, &n, v2.data()));
                out("Config %d recommended (XrFoveatedViewConfigurationViewVARJO active):\n", (int)type);
                for (uint32_t i = 0; i < n; ++i)
                    out("  view %u: rec %ux%u max %ux%u\n", i, v2[i].recommendedImageRectWidth, v2[i].recommendedImageRectHeight,
                        v2[i].maxImageRectWidth, v2[i].maxImageRectHeight);
            }
        }
        return true;
    }

    bool CreateDevice() {
        PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
        CHECK(xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq));
        XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        CHECK(getReq(instance, system, &req));
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) { out("DXGI factory failed\n"); return false; }
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            adapter->GetDesc1(&d);
            if (memcmp(&d.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0) {
                out("Adapter: %ls\n", d.Description);
                break;
            }
            adapter->Release();
            adapter = nullptr;
        }
        factory->Release();
        if (!adapter) { out("Adapter LUID not found\n"); return false; }
        D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device, nullptr, &context);
        adapter->Release();
        if (FAILED(hr)) { out("D3D11CreateDevice failed 0x%08x\n", (unsigned)hr); return false; }
        return true;
    }

    bool CreateSession() {
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = device;
        XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO, &binding};
        sci.systemId = system;
        CHECK(xrCreateSession(instance, &sci, &session));
        XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        rs.poseInReferenceSpace.orientation.w = 1;
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        CHECK(xrCreateReferenceSpace(session, &rs, &local));
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        CHECK(xrCreateReferenceSpace(session, &rs, &view));
        if (hasGaze) {
            XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
            strcpy_s(asci.actionSetName, "probe");
            strcpy_s(asci.localizedActionSetName, "probe");
            CHECK(xrCreateActionSet(instance, &asci, &actionSet));
            XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
            aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
            strcpy_s(aci.actionName, "gaze");
            strcpy_s(aci.localizedActionName, "gaze");
            CHECK(xrCreateAction(actionSet, &aci, &gazeAction));
            XrPath profile{}, path{};
            CHECK(xrStringToPath(instance, "/interaction_profiles/ext/eye_gaze_interaction", &profile));
            CHECK(xrStringToPath(instance, "/user/eyes_ext/input/gaze_ext/pose", &path));
            XrActionSuggestedBinding b{gazeAction, path};
            XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            sb.interactionProfile = profile;
            sb.countSuggestedBindings = 1;
            sb.suggestedBindings = &b;
            XrResult r = xrSuggestInteractionProfileBindings(instance, &sb);
            out("Suggest eye gaze binding -> %d\n", (int)r);
            XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
            at.countActionSets = 1;
            at.actionSets = &actionSet;
            CHECK(xrAttachSessionActionSets(session, &at));
            XrActionSpaceCreateInfo sp{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            sp.action = gazeAction;
            sp.poseInActionSpace.orientation.w = 1;
            CHECK(xrCreateActionSpace(session, &sp, &gazeSpace));
        }
        return true;
    }

    void PollEvents() {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto* s = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                state = s->state;
                out("[event] session state -> %d\n", (int)state);
                if (state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
                    XrResult r = xrBeginSession(session, &bi);
                    out("xrBeginSession(QUAD) -> %d\n", (int)r);
                    running = XR_SUCCEEDED(r);
                } else if (state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session);
                    running = false;
                } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                    quit = true;
                }
            } else {
                out("[event] type %d\n", (int)ev.type);
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    static void Tan(const XrFovf& f, double& w, double& h, double& cx, double& cy) {
        double l = std::tan(f.angleLeft), r = std::tan(f.angleRight), u = std::tan(f.angleUp), d = std::tan(f.angleDown);
        w = r - l;
        h = u - d;
        cx = (r + l) / 2;
        cy = (u + d) / 2;
    }

    void Locate(XrTime time, bool foveated) {
        XrViewLocateFoveatedRenderingVARJO fr{XR_TYPE_VIEW_LOCATE_FOVEATED_RENDERING_VARJO};
        fr.foveatedRenderingActive = XR_TRUE;
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO, foveated ? &fr : nullptr};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
        li.displayTime = time;
        li.space = view;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        XrView views[4]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t n = 0;
        XrResult r = xrLocateViews(session, &li, &vs, 4, &n, views);
        out("  xrLocateViews(QUAD, foveated=%d) -> %d, %u views, flags 0x%llx\n", foveated, (int)r, n, (unsigned long long)vs.viewStateFlags);
        if (XR_FAILED(r) || n < 4) return;
        double sw[2], sh[2];
        double sa[2], sv[2];
        for (uint32_t i = 0; i < 4; ++i) {
            const XrFovf& f = views[i].fov;
            double w, h, cx, cy;
            Tan(f, w, h, cx, cy);
            double hAng = (f.angleRight - f.angleLeft) * kDeg, vAng = (f.angleUp - f.angleDown) * kDeg;
            out("    view %u: L=%7.2f R=%7.2f U=%7.2f D=%7.2f deg | span H=%6.2f V=%6.2f deg | tan L=%.4f R=%.4f U=%.4f D=%.4f (w=%.4f h=%.4f c=%.4f,%.4f)",
                i, f.angleLeft * kDeg, f.angleRight * kDeg, f.angleUp * kDeg, f.angleDown * kDeg, hAng, vAng, std::tan(f.angleLeft),
                std::tan(f.angleRight), std::tan(f.angleUp), std::tan(f.angleDown), w, h, cx, cy);
            out(" pos=(%.4f,%.4f,%.4f)", views[i].pose.position.x, views[i].pose.position.y, views[i].pose.position.z);
            if (i < 2) {
                sw[i] = w; sh[i] = h; sa[i] = hAng; sv[i] = vAng;
                out("\n");
            } else {
                uint32_t e = i - 2;
                out("\n      -> focus/stereo eye %u: tan width %.3f height %.3f area %.3f | angle width %.3f height %.3f\n", e, w / sw[e], h / sh[e],
                    (w * h) / (sw[e] * sh[e]), hAng / sa[e], vAng / sv[e]);
                if (plainViews.size() == 4) {
                    double pxW = plainViews[i].recommendedImageRectWidth / w, pxH = plainViews[i].recommendedImageRectHeight / h;
                    double spW = plainViews[e].recommendedImageRectWidth / sw[e], spH = plainViews[e].recommendedImageRectHeight / sh[e];
                    out("      -> px per tan unit: focus %.1f x %.1f, stereo %.1f x %.1f, density ratio %.3f x %.3f\n", pxW, pxH, spW, spH,
                        pxW / spW, pxH / spH);
                }
            }
        }
    }

    void Gaze(XrTime time) {
        if (!hasGaze || state != XR_SESSION_STATE_FOCUSED) return;
        XrActiveActionSet active{actionSet, XR_NULL_PATH};
        XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
        si.countActiveActionSets = 1;
        si.activeActionSets = &active;
        xrSyncActions(session, &si);
    }

    void PrintGaze(XrTime time) {
        if (!hasGaze) return;
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = gazeAction;
        XrActionStatePose ps{XR_TYPE_ACTION_STATE_POSE};
        xrGetActionStatePose(session, &gi, &ps);
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        XrResult r = xrLocateSpace(gazeSpace, view, time, &loc);
        if (XR_FAILED(r) || !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            out("  gaze: active=%d not valid (r=%d flags=0x%llx)\n", ps.isActive, (int)r, (unsigned long long)loc.locationFlags);
            return;
        }
        const XrQuaternionf& q = loc.pose.orientation;
        // Forward = rotate (0,0,-1).
        double fx = -(2 * (q.x * q.z + q.w * q.y)), fy = -(2 * (q.y * q.z - q.w * q.x)), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
        out("  gaze: active=%d dir=(%.3f,%.3f,%.3f) yaw=%.2f pitch=%.2f deg\n", ps.isActive, fx, fy, fz, std::atan2(fx, -fz) * kDeg,
            std::atan2(fy, -fz) * kDeg);
    }

    bool Run(double seconds) {
        auto start = std::chrono::steady_clock::now();
        auto lastSample = start - std::chrono::seconds(2);
        int frames = 0;
        while (!quit) {
            PollEvents();
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - start).count();
            if (elapsed > seconds + 15 && !running) { out("Session never started running\n"); break; }
            if (!running) { Sleep(10); continue; }
            if (elapsed > seconds + 20) break;
            XrFrameState fs{XR_TYPE_FRAME_STATE};
            XrResult r = xrWaitFrame(session, nullptr, &fs);
            if (XR_FAILED(r)) { out("xrWaitFrame -> %d\n", (int)r); break; }
            r = xrBeginFrame(session, nullptr);
            if (XR_FAILED(r)) { out("xrBeginFrame -> %d\n", (int)r); break; }
            Gaze(fs.predictedDisplayTime);
            if (std::chrono::duration<double>(now - lastSample).count() >= 1.0) {
                lastSample = now;
                out("t=%.2fs state=%d frame=%d shouldRender=%d\n", elapsed, (int)state, frames, fs.shouldRender);
                Locate(fs.predictedDisplayTime, false);
                if (hasFoveated) Locate(fs.predictedDisplayTime, true);
                PrintGaze(fs.predictedDisplayTime);
            }
            XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
            ei.displayTime = fs.predictedDisplayTime;
            ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            r = xrEndFrame(session, &ei);
            if (XR_FAILED(r)) { out("xrEndFrame -> %d\n", (int)r); break; }
            ++frames;
            if (state == XR_SESSION_STATE_FOCUSED || state == XR_SESSION_STATE_VISIBLE || state == XR_SESSION_STATE_SYNCHRONIZED) {
                static double runStart = -1;
                if (runStart < 0) runStart = elapsed;
                if (elapsed - runStart > seconds) break;
            }
        }
        out("Frames submitted: %d\n", frames);
        return true;
    }

    void Shutdown() {
        if (session) {
            if (running) {
                XrResult r = xrRequestExitSession(session);
                out("xrRequestExitSession -> %d\n", (int)r);
                auto t0 = std::chrono::steady_clock::now();
                while (running && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
                    PollEvents();
                    if (running) {
                        // Keep the frame loop alive until STOPPING arrives.
                        XrFrameState fs{XR_TYPE_FRAME_STATE};
                        if (XR_FAILED(xrWaitFrame(session, nullptr, &fs))) break;
                        xrBeginFrame(session, nullptr);
                        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
                        ei.displayTime = fs.predictedDisplayTime;
                        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                        xrEndFrame(session, &ei);
                    }
                }
                if (running) { out("xrEndSession (forced) -> %d\n", (int)xrEndSession(session)); running = false; }
            }
            if (gazeSpace) xrDestroySpace(gazeSpace);
            if (view) xrDestroySpace(view);
            if (local) xrDestroySpace(local);
            out("xrDestroySession -> %d\n", (int)xrDestroySession(session));
        }
        if (actionSet) xrDestroyActionSet(actionSet);
        if (instance) out("xrDestroyInstance -> %d\n", (int)xrDestroyInstance(instance));
        if (context) context->Release();
        if (device) device->Release();
    }
};

int main(int argc, char** argv) {
    const char* logPath = argc > 1 ? argv[1] : nullptr;
    if (logPath) fopen_s(&g_log, logPath, "w");
    double seconds = argc > 2 ? atof(argv[2]) : 5.0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    out("quad_fov_probe %04d-%02d-%02d %02d:%02d:%02d\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    char rt[1024] = {};
    GetEnvironmentVariableA("XR_RUNTIME_JSON", rt, sizeof(rt));
    out("XR_RUNTIME_JSON=%s\n", rt);
    Probe p;
    bool ok = p.Create() && p.PrintViewConfigs() && p.CreateDevice() && p.CreateSession() && p.Run(seconds);
    p.Shutdown();
    out("RESULT %s\n", ok ? "OK" : "FAILED");
    if (g_log) fclose(g_log);
    return ok ? 0 : 1;
}
