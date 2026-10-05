#include "dlss_nr.hpp"
#include "dlss_nr_lifetime.hpp"
#include "runtime.hpp"
#include "openvr_gaze.hpp"
#include "libovr_gaze.hpp"
#include <d3dcompiler.h>
#include "xrfg/d3d12_frame_synthesizer.hpp"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <fstream>
#include <numeric>
#include <chrono>
#include <iomanip>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace cheeky::foveated_dlss;
namespace {
HMODULE core{};
bool quiet_trace{};
void check(HRESULT r) { if (FAILED(r)) throw std::runtime_error("D3D12 failure " + std::to_string(unsigned(r))); }
void require(bool v, const char* s) { if (!v) throw std::runtime_error(s); }
UINT width = 512, height = 512, stereo_extent = 512;
struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    UINT64 tick{};
    Gpu() {
        ComPtr<IDXGIFactory6> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i=0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
            if (desc.VendorId != 0x10DE || desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { adapter.Reset(); continue; }
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
                std::wcout << L"Hardware: " << desc.Description << L'\n'; break;
            }
            adapter.Reset();
        }
        require(device != nullptr, "No NVIDIA hardware D3D12 device; no software fallback permitted");
        D3D12_COMMAND_QUEUE_DESC q{}; check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    }
    ~Gpu() { CloseHandle(event); }
    void submit() {
        check(list->Close()); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
        nr_recording_submitted(queue.Get(), list.Get());
        check(queue->Signal(fence.Get(), ++tick)); check(fence->SetEventOnCompletion(tick, event));
        require(WaitForSingleObject(event, 60000) == WAIT_OBJECT_0, "GPU work timed out");
        check(device->GetDeviceRemovedReason());
        check(allocator->Reset()); check(list->Reset(allocator.Get(), nullptr));
        nr_recording_reset(list.Get(), S_OK); collect_dlss_nr_submissions();
    }
    ComPtr<ID3D12Resource> buffer(UINT64 bytes, D3D12_HEAP_TYPE type) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; d.Width=bytes; d.Height=1; d.DepthOrArraySize=1; d.MipLevels=1; d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r; check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r))); return r;
    }
    ComPtr<ID3D12Resource> texture(DXGI_FORMAT format, const std::vector<float>& pixels) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=width; d.Height=height; d.DepthOrArraySize=1; d.MipLevels=1; d.Format=format; d.SampleDesc.Count=1; d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> r; check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)));
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 bytes{}; device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
        auto upload=buffer(bytes,D3D12_HEAP_TYPE_UPLOAD); void* mapped{}; check(upload->Map(0,nullptr,&mapped));
        auto row=pixels.size()*sizeof(float)/height;
        for(UINT y=0;y<height;++y) memcpy(static_cast<char*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch,reinterpret_cast<const char*>(pixels.data())+y*row,row);
        upload->Unmap(0,nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst{r.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; D3D12_TEXTURE_COPY_LOCATION src{upload.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; src.PlacedFootprint=fp;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition={r.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS}; list->ResourceBarrier(1,&b); submit(); return r;
    }
    std::vector<float> read(ID3D12Resource* r) {
        auto d=r->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 bytes{}; device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
        auto readback=buffer(bytes,D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE}; list->ResourceBarrier(1,&b);
        D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; dst.PlacedFootprint=fp; D3D12_TEXTURE_COPY_LOCATION src{r,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter); list->ResourceBarrier(1,&b); submit();
        std::vector<float> out(width*height*4); void* mapped{}; check(readback->Map(0,nullptr,&mapped));
        for(UINT y=0;y<height;++y) memcpy(out.data()+y*width*4,static_cast<char*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch,width*4*sizeof(float));
        readback->Unmap(0,nullptr); return out;
    }
};
#include "combined_gpu.inc"
#include "performance_gpu.inc"
#include "nr_inplace_gpu.inc"
}

namespace cheeky::foveated_dlss {
HMODULE find_loaded_ngx_core_runtime() noexcept { return core; }
void trace_event(const char* fmt, ...) noexcept { if (quiet_trace) return; va_list a; va_start(a,fmt); vprintf(fmt,a); va_end(a); puts(""); }
void log_info(const char* m) noexcept { puts(m); }
void log_warning(const char* m) noexcept { puts(m); }
void log_error(const char* m) noexcept { puts(m); }
// Hardware timing and native headset gaze are outside this offscreen fixture.
void note_d3d12_command_list_submission(ID3D12CommandQueue*, ID3D12GraphicsCommandList*) noexcept {}
void note_d3d12_command_list_reset(ID3D12GraphicsCommandList*) noexcept {}
void note_d3d12_present(ID3D12CommandQueue*) noexcept {}
bool read_openvr_gaze(const Settings&, IUnknown*, CheekyGazeSnapshotV1&, std::uint64_t) noexcept { return false; }
bool read_libovr_gaze(const Settings&, IUnknown*, CheekyGazeSnapshotV1&, std::uint64_t) noexcept { return false; }
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    try {
        const bool benchmark = argc == 9 && std::wstring_view(argv[3]) == L"--benchmark";
        require(argc == 2 || argc == 3 || benchmark, "Usage: neural_gpu <NGX core> [workspace] [--benchmark report.json focusDimension stereoDimension neuralScale flowScale,preset[,bidirectional]]");
        if (benchmark) {
            width=height=std::stoul(argv[5]); stereo_extent=std::stoul(argv[6]); quiet_trace=true;
            require(width>=256 && width<=4096 && stereo_extent>=width && stereo_extent<=6144,"Invalid benchmark dimensions");
        }
        core=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        require(core != nullptr, "Could not load NVIDIA NGX parameter allocator");
        Gpu gpu;
        auto depth=gpu.texture(DXGI_FORMAT_R32_FLOAT,std::vector<float>(width*height,.5f));
        auto motion=gpu.texture(DXGI_FORMAT_R32G32_FLOAT,std::vector<float>(width*height*2,0));
        Settings settings; settings.enabled=false; settings.nr_enabled=true; settings.nr_foveated=false; settings.nr_use_sr_foveation=false; settings.auto_stereo_alignment=false; settings.nr_intensity=.35f; settings.nr_working_scale=.75f;
        std::array<ComPtr<ID3D12Resource>,2> colors;
        std::array<std::vector<float>,2> originals;
        for(unsigned eye=0;eye<2;++eye) {
            auto& pixels=originals[eye]; pixels.resize(width*height*4);
            for(UINT y=0;y<height;++y) for(UINT x=0;x<width;++x) {
                const auto p=(y*width+x)*4;
                pixels[p]=.05f+.7f*x/width; pixels[p+1]=.1f+.6f*y/height;
                pixels[p+2]=(((x+eye*8)/32+y/32)%2)*.5f+.1f; pixels[p+3]=1;
            }
            colors[eye]=gpu.texture(DXGI_FORMAT_R32G32B32A32_FLOAT,pixels);
        }
        if (benchmark) {
            run_performance_gpu(gpu, colors, argv[2], settings, depth.Get(), motion.Get(), std::stof(argv[7]), argv[8], argv[4]);
            release_dlss_nr_resources(); return 0;
        }
        for(unsigned cycle=0;cycle<3;++cycle) for(unsigned eye=0;eye<2;++eye) {
            DlssNrFrame f{}; f.view_id=2+eye; f.route=DlssNrRoute::d3d12_native; f.command_list=gpu.list.Get(); f.color=colors[eye].Get(); f.depth=depth.Get(); f.motion_vectors=motion.Get();
            f.input_width=f.output_width=f.depth_width=f.motion_width=width; f.input_height=f.output_height=f.depth_height=f.motion_height=height; f.depth_state=f.motion_state=D3D12_RESOURCE_STATE_UNORDERED_ACCESS; f.reset=cycle==0;
            auto ok=evaluate_dlss_nr(f,settings); auto s=dlss_nr_snapshot();
            std::cout<<"Eye "<<eye<<" frame "<<cycle<<" NR="<<ok<<" state="<<dlss_nr_state_name(s.state)<<" result=0x"<<std::hex<<s.last_result<<std::dec<<" evaluations="<<s.evaluation_calls<<" failures="<<s.failed_calls<<" reason="<<(s.skip_reason?s.skip_reason:"none")<<'\n';
            gpu.submit(); require(ok && s.state==DlssNrState::active && s.failed_calls==0,"Real neural evaluation failed");
        }
        for(unsigned eye=0;eye<2;++eye) {
            auto pixels=gpu.read(colors[eye].Get()); double difference{}; size_t changed{};
            for(size_t i=0;i<pixels.size();++i) { require(std::isfinite(pixels[i]),"Non-finite neural output"); auto delta=std::abs(double(pixels[i])-originals[eye][i]); difference+=delta; changed+=delta>1e-5; }
            std::cout<<"Eye "<<eye<<" changed_components="<<changed<<" mean_absolute_change="<<difference/pixels.size()<<'\n';
            require(changed>width*height/10 && difference/pixels.size()>1e-5,"Neural output is indistinguishable from passthrough");
        }
        if (argc == 3) verify_combined_gpu(gpu, colors, argv[2], settings, depth.Get(), motion.Get());
        const auto final=dlss_nr_snapshot();
        require(final.failed_calls==0 && final.evaluation_calls==(argc==3?12u:6u),"Unexpected final neural counters");
        if (argc == 3) nr_inplace::verify(gpu, settings);
        release_dlss_nr_resources(); std::cout<<"PASS: real NVIDIA feature 18, two separate view histories, "<<final.evaluation_calls<<" evaluations and finite changed pixels\n"; return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
}
