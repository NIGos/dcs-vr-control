#pragma once
// DCS VR Control: plain bilinear upscale of a DLSS colour input into the DLSS
// output, for the few loading frames in which a deferred Quad Views focus
// feature cannot be classified yet (no Quad Views layout published). It keeps
// the headset image sensible without creating DCS's own DX11 DLSS feature
// (whose VRAM NGX keeps until shutdown). Any failure returns false and the
// caller falls back to DCS's own DLSS as before.
#include "d3d11_write_bindings.hpp"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace cheeky::foveated_dlss {
struct DcsHoldUpscaleRegion {
    std::uint32_t source_x{}, source_y{}, source_width{}, source_height{};
    std::uint32_t target_x{}, target_y{}, target_width{}, target_height{};
};

inline constexpr char dcs_hold_upscale_source[] = R"(
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Target : register(u0);
SamplerState Bilinear : register(s0);
cbuffer Region : register(b0) { float4 source_rect; float4 texel; uint4 target_rect; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= target_rect.z || id.y >= target_rect.w) return;
    float2 p = source_rect.xy + (float2(id.xy) + 0.5) * source_rect.zw / float2(target_rect.zw);
    p = clamp(p, source_rect.xy + 0.5, source_rect.xy + source_rect.zw - 0.5);
    Target[target_rect.xy + id.xy] = Source.SampleLevel(Bilinear, p * texel.xy, 0);
}
)";

// One cached shader set for the device DCS renders with (it never changes in a session).
struct DcsHoldUpscaleCache {
    std::mutex mutex;
    ID3D11Device* device{};
    ID3D11ComputeShader* shader{};
    ID3D11SamplerState* sampler{};
    ID3D11Buffer* constants{};
    bool failed{};
    void reset() noexcept {
        if (constants) constants->Release();
        if (sampler) sampler->Release();
        if (shader) shader->Release();
        if (device) device->Release();
        constants = nullptr; sampler = nullptr; shader = nullptr; device = nullptr; failed = false;
    }
};
inline DcsHoldUpscaleCache& dcs_hold_upscale_cache() noexcept { static DcsHoldUpscaleCache cache; return cache; }

[[nodiscard]] inline bool dcs_hold_upscale_prepare(DcsHoldUpscaleCache& cache, ID3D11Device* device) noexcept {
    if (cache.device == device) return !cache.failed;
    cache.reset();
    device->AddRef();
    cache.device = device;
    ID3DBlob* code{};
    ID3DBlob* errors{};
    const auto compiled = D3DCompile(dcs_hold_upscale_source, sizeof(dcs_hold_upscale_source) - 1U, "dcs_hold_upscale",
        nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0U, &code, &errors);
    if (errors) errors->Release();
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = 48U;
    constants.Usage = D3D11_USAGE_DYNAMIC;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cache.failed = FAILED(compiled) || code == nullptr ||
        FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cache.shader)) ||
        FAILED(device->CreateSamplerState(&sampler, &cache.sampler)) ||
        FAILED(device->CreateBuffer(&constants, nullptr, &cache.constants));
    if (code) code->Release();
    return !cache.failed;
}

[[nodiscard]] inline bool dcs_hold_upscale(ID3D11DeviceContext* context, ID3D11Resource* color,
    ID3D11Resource* output, const DcsHoldUpscaleRegion& region) noexcept {
    if (!context || !color || !output || !region.source_width || !region.source_height ||
        !region.target_width || !region.target_height) return false;
    ID3D11Texture2D* source_texture{};
    ID3D11Texture2D* target_texture{};
    const auto release_textures = [&] {
        if (target_texture) target_texture->Release();
        if (source_texture) source_texture->Release();
    };
    if (FAILED(color->QueryInterface(IID_PPV_ARGS(&source_texture))) ||
        FAILED(output->QueryInterface(IID_PPV_ARGS(&target_texture)))) { release_textures(); return false; }
    D3D11_TEXTURE2D_DESC source_desc{}, target_desc{};
    source_texture->GetDesc(&source_desc);
    target_texture->GetDesc(&target_desc);
    if (source_desc.SampleDesc.Count != 1U || target_desc.SampleDesc.Count != 1U ||
        !(target_desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) || !(source_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
        std::uint64_t(region.source_x) + region.source_width > source_desc.Width ||
        std::uint64_t(region.source_y) + region.source_height > source_desc.Height ||
        std::uint64_t(region.target_x) + region.target_width > target_desc.Width ||
        std::uint64_t(region.target_y) + region.target_height > target_desc.Height) { release_textures(); return false; }
    ID3D11Device* device{};
    context->GetDevice(&device);
    auto& cache = dcs_hold_upscale_cache();
    std::lock_guard lock(cache.mutex);
    const bool ready = device && dcs_hold_upscale_prepare(cache, device);
    ID3D11ShaderResourceView* srv{};
    ID3D11UnorderedAccessView* uav{};
    // Single-slice textures with typed formats only (DCS: R16G16B16A16_FLOAT); anything else falls back.
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = source_desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1U;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.Format = target_desc.Format;
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const bool bound = ready && source_desc.ArraySize == 1U && target_desc.ArraySize == 1U &&
        SUCCEEDED(device->CreateShaderResourceView(source_texture, &srv_desc, &srv)) &&
        SUCCEEDED(device->CreateUnorderedAccessView(target_texture, &uav_desc, &uav)) &&
        SUCCEEDED(context->Map(cache.constants, 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped));
    if (bound) {
        const float source_rect[8]{float(region.source_x), float(region.source_y), float(region.source_width),
            float(region.source_height), 1.0F / float(source_desc.Width), 1.0F / float(source_desc.Height), 0.0F, 0.0F};
        const std::uint32_t target_rect[4]{region.target_x, region.target_y, region.target_width, region.target_height};
        std::memcpy(mapped.pData, source_rect, sizeof(source_rect));
        std::memcpy(static_cast<char*>(mapped.pData) + sizeof(source_rect), target_rect, sizeof(target_rect));
        context->Unmap(cache.constants, 0U);
        D3D11WriteBindingsScope write_bindings(context);
        ID3D11ComputeShader* old_shader{};
        ID3D11ShaderResourceView* old_srv{};
        ID3D11UnorderedAccessView* old_uav{};
        ID3D11SamplerState* old_sampler{};
        ID3D11Buffer* old_buffer{};
        context->CSGetShader(&old_shader, nullptr, nullptr);
        context->CSGetShaderResources(0U, 1U, &old_srv);
        context->CSGetUnorderedAccessViews(0U, 1U, &old_uav);
        context->CSGetSamplers(0U, 1U, &old_sampler);
        context->CSGetConstantBuffers(0U, 1U, &old_buffer);
        context->CSSetShader(cache.shader, nullptr, 0U);
        context->CSSetShaderResources(0U, 1U, &srv);
        context->CSSetUnorderedAccessViews(0U, 1U, &uav, nullptr);
        context->CSSetSamplers(0U, 1U, &cache.sampler);
        context->CSSetConstantBuffers(0U, 1U, &cache.constants);
        context->Dispatch((region.target_width + 7U) / 8U, (region.target_height + 7U) / 8U, 1U);
        ID3D11ShaderResourceView* null_srv{};
        ID3D11UnorderedAccessView* null_uav{};
        context->CSSetShaderResources(0U, 1U, &null_srv);
        context->CSSetUnorderedAccessViews(0U, 1U, &null_uav, nullptr);
        context->CSSetShader(old_shader, nullptr, 0U);
        context->CSSetShaderResources(0U, 1U, &old_srv);
        const UINT keep_counter = static_cast<UINT>(-1);
        context->CSSetUnorderedAccessViews(0U, 1U, &old_uav, &keep_counter);
        context->CSSetSamplers(0U, 1U, &old_sampler);
        context->CSSetConstantBuffers(0U, 1U, &old_buffer);
        if (old_buffer) old_buffer->Release();
        if (old_sampler) old_sampler->Release();
        if (old_uav) old_uav->Release();
        if (old_srv) old_srv->Release();
        if (old_shader) old_shader->Release();
    }
    if (uav) uav->Release();
    if (srv) srv->Release();
    if (device) device->Release();
    release_textures();
    return bound;
}
} // namespace cheeky::foveated_dlss
