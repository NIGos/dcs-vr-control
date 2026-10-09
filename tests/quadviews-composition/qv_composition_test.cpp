// DCS VR Control: the bundled Quad Views composition, as built by
// scripts/build-quadviews.ps1, against the same shaders without our
// performance edits (the early out of the composition and the sharpening's
// skipped tiles). Every pixel of the composited image must be identical, for
// the round and the rectangular focus area, with and without sharpening, with
// the focus area centred and off centre. Exit code 0 on success.
//
//   qv_composition_test.exe <quad views openxr-api-layer dir> <FidelityFX-CAS ffx-cas dir>

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

void replace_once(std::string& text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    if (at == std::string::npos) throw std::runtime_error("anchor not found: " + from);
    text.replace(at, from.size(), to);
}

class Includes final : public ID3DInclude {
public:
    explicit Includes(std::vector<std::filesystem::path> directories) : directories_(std::move(directories)) {}
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override {
        for (const auto& directory : directories_) {
            const auto path = directory / name;
            if (std::filesystem::exists(path)) {
                auto* text = new std::string(read_text(path));
                *data = text->data();
                *bytes = static_cast<UINT>(text->size());
                held_.push_back(text);
                return S_OK;
            }
        }
        return E_FAIL;
    }
    HRESULT __stdcall Close(LPCVOID) override { return S_OK; }
    ~Includes() { for (auto* text : held_) delete text; }

private:
    std::vector<std::filesystem::path> directories_;
    std::vector<std::string*> held_;
};

ComPtr<ID3DBlob> compile(const std::string& source, const char* entry, const char* target, Includes& includes,
    const std::vector<D3D_SHADER_MACRO>& macros = {}) {
    std::vector<D3D_SHADER_MACRO> defined = macros;
    defined.push_back({nullptr, nullptr});
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source.data(), source.size(), "shader", defined.data(), &includes, entry, target,
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
        throw std::runtime_error(std::string("compile failed: ") +
            (errors ? static_cast<const char*>(errors->GetBufferPointer()) : entry));
    }
    return code;
}

void check(HRESULT result, const char* what) {
    if (FAILED(result)) throw std::runtime_error(what);
}

struct Case {
    float shape;
    float smoothing;
    float offset_x, offset_y;
    bool sharpen;
    bool unpremultiplied;
    float taper = 0.f;  // dcsvr_sharpen_taper band (0: off)
};

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3) throw std::runtime_error("usage: qv_composition_test <openxr-api-layer dir> <ffx-cas dir>");
        const std::filesystem::path layer_dir = argv[1];
        const std::filesystem::path cas_dir = argv[2];
        Includes includes({layer_dir, cas_dir});

        const std::string projection_new = read_text(layer_dir / "ProjectionPS.hlsl");
        std::string projection_old = projection_new;
        replace_once(projection_old, "[branch] if (!focusVisible && !debugFocusView) {", "[branch] if (false) {");
        replace_once(projection_old, "sourceFocusTexture.SampleLevel(sourceSampler, layer1TexCoord, 0)",
            "sourceFocusTexture.Sample(sourceSampler, layer1TexCoord)");
        const std::string vertex = read_text(layer_dir / "ProjectionVS.hlsl");
        const std::string sharpening = read_text(layer_dir / "SharpeningCS.hlsl");

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL level{};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                &device, &level, &context))) {
            check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                      &device, &level, &context), "device");
        }

        const auto vs_code = compile(vertex, "main", "vs_5_0", includes);
        const auto ps_new_code = compile(projection_new, "main", "ps_5_0", includes);
        const auto ps_old_code = compile(projection_old, "main", "ps_5_0", includes);
        const auto cs_code = compile(sharpening, "main", "cs_5_0", includes,
            {{"CAS_SAMPLE_FP16", "0"}, {"CAS_SAMPLE_SHARPEN_ONLY", "1"}, {"WIDTH", "64"}, {"HEIGHT", "1"}, {"DEPTH", "1"}});
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps_new, ps_old;
        ComPtr<ID3D11ComputeShader> cs;
        check(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs), "vs");
        check(device->CreatePixelShader(ps_new_code->GetBufferPointer(), ps_new_code->GetBufferSize(), nullptr, &ps_new), "ps new");
        check(device->CreatePixelShader(ps_old_code->GetBufferPointer(), ps_old_code->GetBufferSize(), nullptr, &ps_old), "ps old");
        check(device->CreateComputeShader(cs_code->GetBufferPointer(), cs_code->GetBufferSize(), nullptr, &cs), "cs");

        // QV_BENCH=1: the composition's GPU time at a Crystal Super's size instead of the equality test.
        const bool bench = GetEnvironmentVariableA("QV_BENCH", nullptr, 0) != 0;
        const UINT kOut = bench ? 5400U : 1024U;
        const UINT kFocus = bench ? 2076U : 600U;
        std::mt19937 random(1234);
        const auto make_texture = [&](UINT size, DXGI_FORMAT format, UINT bind, bool fill) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = desc.Height = size;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = bind;
            std::vector<std::uint32_t> pixels(static_cast<std::size_t>(size) * size);
            for (auto& p : pixels) p = random();
            D3D11_SUBRESOURCE_DATA data{pixels.data(), size * 4U, 0};
            ComPtr<ID3D11Texture2D> texture;
            check(device->CreateTexture2D(&desc, fill ? &data : nullptr, &texture), "texture");
            return texture;
        };
        const auto stereo = make_texture(kOut, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, true);
        const auto focus = make_texture(kFocus, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, true);
        D3D11_TEXTURE2D_DESC sharpened_desc{};
        sharpened_desc.Width = sharpened_desc.Height = kFocus;
        sharpened_desc.MipLevels = sharpened_desc.ArraySize = 1;
        sharpened_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sharpened_desc.SampleDesc.Count = 1;
        sharpened_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> sharpened;
        check(device->CreateTexture2D(&sharpened_desc, nullptr, &sharpened), "sharpened");
        D3D11_TEXTURE2D_DESC target_desc{};
        target_desc.Width = target_desc.Height = kOut;
        target_desc.MipLevels = target_desc.ArraySize = 1;
        target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        target_desc.SampleDesc.Count = 1;
        target_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target, staging;
        check(device->CreateTexture2D(&target_desc, nullptr, &target), "target");
        target_desc.BindFlags = 0;
        target_desc.Usage = D3D11_USAGE_STAGING;
        target_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&target_desc, nullptr, &staging), "staging");

        ComPtr<ID3D11ShaderResourceView> stereo_srv, focus_srv, sharpened_srv;
        ComPtr<ID3D11UnorderedAccessView> sharpened_uav;
        ComPtr<ID3D11RenderTargetView> rtv;
        check(device->CreateShaderResourceView(stereo.Get(), nullptr, &stereo_srv), "srv");
        check(device->CreateShaderResourceView(focus.Get(), nullptr, &focus_srv), "srv");
        check(device->CreateShaderResourceView(sharpened.Get(), nullptr, &sharpened_srv), "srv");
        check(device->CreateUnorderedAccessView(sharpened.Get(), nullptr, &sharpened_uav), "uav");
        check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv), "rtv");

        D3D11_SAMPLER_DESC sampler_desc{};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        check(device->CreateSamplerState(&sampler_desc, &sampler), "sampler");
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        ComPtr<ID3D11RasterizerState> rasterizer;
        check(device->CreateRasterizerState(&raster, &rasterizer), "raster");

        const auto buffer = [&](UINT size) {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = size;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            ComPtr<ID3D11Buffer> created;
            check(device->CreateBuffer(&desc, nullptr, &created), "buffer");
            return created;
        };
        const auto vs_constants = buffer(64), ps_constants = buffer(32), cs_constants = buffer(64);

        const auto run = [&](const Case& c, bool optimized) {
            // Sharpening (skipping tiles only in the optimized chain).
            ID3D11ShaderResourceView* focus_view = focus_srv.Get();
            if (c.sharpen) {
                const float zero[4]{};
                context->ClearUnorderedAccessViewFloat(sharpened_uav.Get(), zero);
                // CAS sharpen-only setup at 0.7 (CasSetup's output, precomputed for 600x600 at sharpness 0.7).
                struct { std::uint32_t c0[4]; std::uint32_t c1[4]; float skip[4]; float taper[4]; } k{};
                const float sharp = -1.0f / (8.0f + (5.0f - 8.0f) * 0.7f);
                const float one = 1.0f;
                std::memcpy(&k.c0[0], &one, 4); std::memcpy(&k.c0[1], &one, 4);
                const float zerof = 0.0f;
                std::memcpy(&k.c0[2], &zerof, 4); std::memcpy(&k.c0[3], &zerof, 4);
                std::memcpy(&k.c1[0], &sharp, 4);
                k.c1[1] = 0;  // half packing unused at FP32
                const float eight = 8.0f * sharp;
                std::memcpy(&k.c1[2], &eight, 4);
                k.c1[3] = 0;
                k.skip[0] = optimized ? c.shape : 0.f;
                k.skip[1] = 2.f;
                k.skip[2] = k.skip[3] = static_cast<float>(kFocus);
                k.taper[0] = c.taper;
                k.taper[1] = c.shape;
                context->UpdateSubresource(cs_constants.Get(), 0, nullptr, &k, 0, 0);
                context->CSSetConstantBuffers(0, 1, cs_constants.GetAddressOf());
                context->CSSetShaderResources(0, 1, focus_srv.GetAddressOf());
                context->CSSetUnorderedAccessViews(0, 1, sharpened_uav.GetAddressOf(), nullptr);
                context->CSSetShader(cs.Get(), nullptr, 0);
                context->Dispatch((kFocus + 15) / 16, (kFocus + 15) / 16, 1);
                ID3D11UnorderedAccessView* none[1]{};
                context->CSSetUnorderedAccessViews(0, 1, none, nullptr);
                focus_view = sharpened_srv.Get();
            }
            // Focus area: 0.34 of the view, possibly off centre.
            const float sx = 1.f / 0.34f, sy = 1.f / 0.34f;
            const float m[4][4] = {{sx, 0, 0, 0}, {0, sy, 0, 0}, {0, 0, 1, 0}, {-c.offset_x * sx, -c.offset_y * sy, 0, 1}};
            float column_major[16];
            for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) column_major[j * 4 + i] = m[i][j];
            context->UpdateSubresource(vs_constants.Get(), 0, nullptr, column_major, 0, 0);
            struct { float smoothing; std::uint32_t ignore_alpha, unpremultiplied, debug; float shape; float pad[3]; } p{};
            p.smoothing = c.smoothing;
            p.ignore_alpha = 0xFFFFFFFEu;
            p.unpremultiplied = c.unpremultiplied ? 1u : 0u;
            p.shape = c.shape;
            context->UpdateSubresource(ps_constants.Get(), 0, nullptr, &p, 0, 0);
            const float clear[4]{0.25f, 0.5f, 0.75f, 1.f};
            context->ClearRenderTargetView(rtv.Get(), clear);
            context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
            D3D11_VIEWPORT viewport{0, 0, static_cast<float>(kOut), static_cast<float>(kOut), 0, 1};
            context->RSSetViewports(1, &viewport);
            context->RSSetState(rasterizer.Get());
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            context->VSSetShader(vs.Get(), nullptr, 0);
            context->VSSetConstantBuffers(0, 1, vs_constants.GetAddressOf());
            context->PSSetShader(optimized ? ps_new.Get() : ps_old.Get(), nullptr, 0);
            context->PSSetConstantBuffers(0, 1, ps_constants.GetAddressOf());
            ID3D11ShaderResourceView* views[2] = {stereo_srv.Get(), focus_view};
            context->PSSetShaderResources(0, 2, views);
            context->PSSetSamplers(0, 1, sampler.GetAddressOf());
            context->Draw(3, 0);
            ID3D11ShaderResourceView* none[2]{};
            context->PSSetShaderResources(0, 2, none);
            if (bench) return std::vector<std::uint32_t>{};
            context->CopyResource(staging.Get(), target.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map");
            std::vector<std::uint32_t> out(static_cast<std::size_t>(kOut) * kOut);
            for (UINT y = 0; y < kOut; ++y)
                std::memcpy(&out[static_cast<std::size_t>(y) * kOut],
                    static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch, kOut * 4);
            context->Unmap(staging.Get(), 0);
            return out;
        };

        if (bench) {
            D3D11_QUERY_DESC disjoint_desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, stamp_desc{D3D11_QUERY_TIMESTAMP, 0};
            ComPtr<ID3D11Query> disjoint, begin, end;
            check(device->CreateQuery(&disjoint_desc, &disjoint), "query");
            check(device->CreateQuery(&stamp_desc, &begin), "query");
            check(device->CreateQuery(&stamp_desc, &end), "query");
            const auto time = [&](const Case& c, bool optimized) {
                std::vector<double> samples;
                for (int i = 0; i < 40; ++i) {
                    context->Begin(disjoint.Get());
                    context->End(begin.Get());
                    static_cast<void>(run(c, optimized));
                    context->End(end.Get());
                    context->End(disjoint.Get());
                    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
                    UINT64 b = 0, e = 0;
                    while (context->GetData(disjoint.Get(), &d, sizeof(d), 0) != S_OK) {}
                    while (context->GetData(begin.Get(), &b, sizeof(b), 0) != S_OK) {}
                    while (context->GetData(end.Get(), &e, sizeof(e), 0) != S_OK) {}
                    if (!d.Disjoint && i >= 5) samples.push_back(double(e - b) * 1000.0 / double(d.Frequency));
                }
                std::sort(samples.begin(), samples.end());
                return samples[samples.size() / 2];
            };
            for (const Case c : {Case{2.f, 0.2f, 0.f, 0.f, true, false}, Case{2.f, 0.2f, 0.f, 0.f, false, false}}) {
                const double before = time(c, false), after = time(c, true);
                std::printf("bench %ux%u focus %u sharpen=%d: composition median before %.3f ms, after %.3f ms%s",
                    kOut, kOut, kFocus, c.sharpen ? 1 : 0, before, after, "\n");
            }
            return 0;
        }
        const std::array<Case, 8> cases{{
            {2.f, 0.2f, 0.f, 0.f, false, false}, {2.f, 0.2f, 0.15f, -0.1f, false, false},
            {2.f, 0.05f, -0.2f, 0.2f, true, false}, {2.f, 0.2f, 0.f, 0.f, true, false},
            {0.f, 0.2f, 0.f, 0.f, false, false}, {0.f, 0.f, 0.1f, 0.1f, true, false},
            {2.f, 0.2f, 0.1f, 0.f, true, true}, {4.f, 0.1f, 0.f, 0.f, true, false}}};
        bool all_identical = true;
        for (std::size_t i = 0; i < cases.size(); ++i) {
            const auto before = run(cases[i], false);
            const auto after = run(cases[i], true);
            std::size_t differing = 0;
            for (std::size_t p = 0; p < before.size(); ++p) differing += before[p] != after[p] ? 1U : 0U;
            std::printf("case %zu shape=%.0f smoothing=%.2f offset=(%.2f,%.2f) sharpen=%d unpremultiplied=%d: %zu differing pixels of %zu\n",
                i, cases[i].shape, cases[i].smoothing, cases[i].offset_x, cases[i].offset_y, cases[i].sharpen ? 1 : 0,
                cases[i].unpremultiplied ? 1 : 0, differing, before.size());
            all_identical = all_identical && differing == 0;
            // The test means something only where the focus view shows: count the pixels the focus changed
            // against the same composition with the focus area pushed out of view.
            Case away = cases[i];
            away.offset_x = 50.f;
            const auto periphery_only = run(away, false);
            std::size_t focus_pixels = 0;
            for (std::size_t p = 0; p < before.size(); ++p) focus_pixels += before[p] != periphery_only[p] ? 1U : 0U;
            std::printf("    focus area pixels %zu%s", focus_pixels, "\n");
            all_identical = all_identical && focus_pixels > before.size() / 20U;
        }
        std::printf(all_identical ? "Quad Views composition identical\n" : "Quad Views composition DIFFERS\n");

        // Sharpening taper (focus radius in the shape's own norm): the same composition in the inner half of the focus area, a softer one in its edge band.
        bool taper_ok = true;
        for (const float shape : {2.f, 8.f}) {
            Case on{shape, 0.2f, 0.f, 0.f, true, false, 0.4f};
            Case off = on;
            off.taper = 0.f;
            const auto plain = run(off, true), tapered = run(on, true);
            const float mid = kOut * 0.5f, half = kOut * 0.5f * 0.34f;
            std::size_t centre = 0, band = 0;
            for (UINT y = 0; y < kOut; ++y)
                for (UINT x = 0; x < kOut; ++x) {
                    const std::size_t p = static_cast<std::size_t>(y) * kOut + x;
                    if (plain[p] == tapered[p]) continue;
                    const float ax = std::fabs(x + 0.5f - mid) / half, ay = std::fabs(y + 0.5f - mid) / half;
                    const float d = std::pow(std::pow(ax, shape) + std::pow(ay, shape), 1.f / shape);
                    if (d < 0.5f) ++centre; else ++band;
                }
            std::printf("taper shape=%.0f: %zu changed pixels in the inner half, %zu in the edge band%s", shape, centre, band, "\n");
            taper_ok = taper_ok && centre == 0 && band > 1000U;
        }
        std::printf(taper_ok ? "Quad Views sharpening taper only softens the edge band%s" : "Quad Views sharpening taper FAILED%s", "\n");
        return all_identical && taper_ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::printf("error: %s\n", error.what());
        return 2;
    }
}
