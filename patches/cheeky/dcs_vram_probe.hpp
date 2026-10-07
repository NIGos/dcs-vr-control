#pragma once
// DCS VR Control: this process's resident video memory (DXGI local segment
// group), for the trace lines that split what the private D3D12 path costs:
// the device and NGX initialization, the private SR feature, the DLSS-NR
// runtime and its feature (search "VRAM_STAGE" in the Cheeky log). The adapter
// is the one with the most dedicated memory, found once.

#include <dxgi1_6.h>

#include <mutex>

namespace cheeky::foveated_dlss {

[[nodiscard]] inline double dcs_process_vram_mb() noexcept {
    static std::once_flag once;
    static IDXGIAdapter3* adapter{};
    std::call_once(once, [] {
        IDXGIFactory1* factory{};
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return;
        SIZE_T best{};
        IDXGIAdapter1* candidate{};
        for (UINT index = 0; factory->EnumAdapters1(index, &candidate) != DXGI_ERROR_NOT_FOUND; ++index) {
            DXGI_ADAPTER_DESC1 desc{};
            IDXGIAdapter3* adapter3{};
            if (SUCCEEDED(candidate->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                desc.DedicatedVideoMemory > best &&
                SUCCEEDED(candidate->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3)))) {
                if (adapter) adapter->Release();
                adapter = adapter3;
                best = desc.DedicatedVideoMemory;
            }
            candidate->Release();
        }
        factory->Release();
    });
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (adapter == nullptr || FAILED(adapter->QueryVideoMemoryInfo(0U, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        return -1.0;
    return static_cast<double>(info.CurrentUsage) / (1024.0 * 1024.0);
}

}  // namespace cheeky::foveated_dlss
