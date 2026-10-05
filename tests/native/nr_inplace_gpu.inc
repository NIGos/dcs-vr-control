// Included after the offscreen Gpu helper. DLSS-NR before upscaling, as the
// DX11->DX12 transport runs it on a DCS focus view (1204x1188 render, 2076x2048
// output, HDR, low-resolution motion), in the two layouts of the transport:
//   separate: SR colour/depth/motion copies plus a second NR colour/depth/motion
//             copy, NR in place on the NR colour, then a copy back into the SR colour;
//   in place: NR reads the SR depth/motion copies and writes into the SR colour.
// Both run the real NVIDIA feature 18 on their own view history over the same
// frame sequence; the resulting SR input must be bit-identical on every frame.
// The GPU time of the copies the in-place layout removes is measured as well.
// DCSVR_NR_DUMP=<file> appends the in-place SR input bytes of every frame (used
// to compare builds, e.g. a different motion-guide format).
namespace nr_inplace {
constexpr UINT render_w = 1204, render_h = 1188, out_w = 2076, out_h = 2048;

std::uint16_t to_half(float value) {
    std::uint32_t bits{}; memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16) & 0x8000U;
    const int exponent = static_cast<int>((bits >> 23) & 0xFFU) - 127 + 15;
    std::uint32_t mantissa = bits & 0x7FFFFFU;
    if (exponent <= 0) return static_cast<std::uint16_t>(sign);
    if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7C00U);
    mantissa += 0x1000U; // round to nearest
    if (mantissa & 0x800000U) { mantissa = 0; if (exponent + 1 >= 31) return static_cast<std::uint16_t>(sign | 0x7C00U);
        return static_cast<std::uint16_t>(sign | ((exponent + 1) << 10)); }
    return static_cast<std::uint16_t>(sign | (exponent << 10) | (mantissa >> 13));
}

ComPtr<ID3D12Resource> make(Gpu& gpu, DXGI_FORMAT format, bool uav, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = render_w; d.Height = render_h;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = format; d.SampleDesc.Count = 1;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r; check(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)));
    return r;
}

void barrier(Gpu& gpu, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    if (a == b) return;
    D3D12_RESOURCE_BARRIER v{}; v.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b}; gpu.list->ResourceBarrier(1, &v);
}

// Records an upload of raw texel rows into r (COPY_DEST); the upload buffer is returned to keep it alive.
ComPtr<ID3D12Resource> upload(Gpu& gpu, ID3D12Resource* r, const std::vector<unsigned char>& bytes, UINT texel) {
    auto d = r->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 size{};
    gpu.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &size);
    auto buffer = gpu.buffer(size, D3D12_HEAP_TYPE_UPLOAD); void* mapped{}; check(buffer->Map(0, nullptr, &mapped));
    for (UINT y = 0; y < render_h; ++y)
        memcpy(static_cast<char*>(mapped) + fp.Offset + y * fp.Footprint.RowPitch, bytes.data() + size_t(y) * render_w * texel, size_t(render_w) * texel);
    buffer->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{r, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src{buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; src.PlacedFootprint = fp;
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    return buffer;
}

std::vector<unsigned char> download(Gpu& gpu, ID3D12Resource* r, D3D12_RESOURCE_STATES state, UINT texel) {
    auto d = r->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 size{};
    gpu.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &size);
    auto readback = gpu.buffer(size, D3D12_HEAP_TYPE_READBACK);
    barrier(gpu, r, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{r, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier(gpu, r, D3D12_RESOURCE_STATE_COPY_SOURCE, state); gpu.submit();
    std::vector<unsigned char> out(size_t(render_w) * render_h * texel); void* mapped{}; check(readback->Map(0, nullptr, &mapped));
    for (UINT y = 0; y < render_h; ++y)
        memcpy(out.data() + size_t(y) * render_w * texel, static_cast<char*>(mapped) + fp.Offset + y * fp.Footprint.RowPitch, size_t(render_w) * texel);
    readback->Unmap(0, nullptr); return out;
}

void copy(Gpu& gpu, ID3D12Resource* dst, ID3D12Resource* src) {
    D3D12_TEXTURE_COPY_LOCATION d{dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, s{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    gpu.list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
}

// A DCS-like focus frame: HDR colour (>1 highlights), reversed-Z depth and
// low-resolution motion in render pixels with sub-pixel fractions.
void frame_data(unsigned frame, std::vector<unsigned char>& color, std::vector<unsigned char>& depth, std::vector<unsigned char>& motion) {
    color.resize(size_t(render_w) * render_h * 8); depth.resize(size_t(render_w) * render_h * 4); motion.resize(size_t(render_w) * render_h * 4);
    const float shift = 1.37f * frame;
    for (UINT y = 0; y < render_h; ++y) for (UINT x = 0; x < render_w; ++x) {
        const size_t i = size_t(y) * render_w + x;
        const float u = (x + shift) / render_w, v = float(y) / render_h;
        const float stripes = ((int(x + shift) / 24 + int(y) / 24) % 2) ? 0.85f : 0.15f;
        const float noise = float((x * 73856093U ^ y * 19349663U ^ frame * 83492791U) % 1000U) / 1000.0f;
        const float rgb[3]{0.05f + 0.6f * u + 0.08f * noise, 0.1f + 0.5f * v * stripes, stripes * (1.0f + 1.5f * (u > 0.8f && v < 0.2f))};
        auto* c = reinterpret_cast<std::uint16_t*>(color.data() + i * 8);
        for (unsigned k = 0; k < 3; ++k) c[k] = to_half(rgb[k]);
        c[3] = to_half(1.0f);
        const float z = 0.02f + 0.9f * (1.0f - v) * (0.7f + 0.3f * stripes);
        memcpy(depth.data() + i * 4, &z, 4);
        auto* m = reinterpret_cast<std::uint16_t*>(motion.data() + i * 4);
        m[0] = to_half(-1.37f + 0.731f * std::sin(0.013f * y + 0.2f * frame));
        m[1] = to_half(0.418f * std::cos(0.011f * x) + 0.0625f * (noise - 0.5f));
    }
}

double ms(UINT64 begin, UINT64 end, UINT64 frequency) { return double(end - begin) * 1000.0 / double(frequency); }

void verify(Gpu& gpu, Settings settings) {
    settings.enabled = false; settings.nr_enabled = true; settings.nr_foveated = false; settings.nr_use_sr_foveation = false;
    settings.nr_processing_order = NrProcessingOrder::before_upscaling; settings.nr_working_scale = 1.0f;
    settings.nr_alignment_border_enabled = false; settings.nr_intensity = 1.0f;
    constexpr auto npsr = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        dest = D3D12_RESOURCE_STATE_COPY_DEST, source = D3D12_RESOURCE_STATE_COPY_SOURCE;
    constexpr auto color_format = DXGI_FORMAT_R16G16B16A16_FLOAT, motion_format = DXGI_FORMAT_R16G16_FLOAT;
    // The game's DX11 inputs (sources of the copies).
    auto game_color = make(gpu, color_format, false, dest), game_depth = make(gpu, DXGI_FORMAT_R32_FLOAT, false, dest),
        game_motion = make(gpu, motion_format, false, dest);
    // Separate layout: SR copies and NR copies. In-place layout: SR copies only (colour with UAV, as patched).
    auto sr_color_a = make(gpu, color_format, false, dest), sr_depth_a = make(gpu, DXGI_FORMAT_R32_FLOAT, true, dest),
        sr_motion_a = make(gpu, motion_format, false, dest);
    auto nr_color = make(gpu, color_format, true, dest), nr_depth = make(gpu, DXGI_FORMAT_R32_FLOAT, true, dest),
        nr_motion = make(gpu, motion_format, false, dest);
    auto sr_color_b = make(gpu, color_format, true, dest), sr_depth_b = make(gpu, DXGI_FORMAT_R32_FLOAT, true, dest),
        sr_motion_b = make(gpu, motion_format, false, dest);
    ComPtr<ID3D12QueryHeap> queries; D3D12_QUERY_HEAP_DESC q{}; q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; q.Count = 8;
    check(gpu.device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)));
    auto times = gpu.buffer(8 * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK);
    UINT64 frequency{}; check(gpu.queue->GetTimestampFrequency(&frequency));
    std::vector<double> removed_copies, separate_nr, in_place_nr;
    wchar_t dump_path[MAX_PATH]{};
    const bool dump = GetEnvironmentVariableW(L"DCSVR_NR_DUMP", dump_path, MAX_PATH) > 0;
    std::ofstream dump_file;
    if (dump) dump_file.open(dump_path, std::ios::binary | std::ios::trunc);
    constexpr unsigned frames = 6;
    for (unsigned frame = 0; frame < frames; ++frame) {
        std::vector<unsigned char> color, depth, motion; frame_data(frame, color, depth, motion);
        if (frame) { barrier(gpu, game_color.Get(), npsr, dest); barrier(gpu, game_depth.Get(), npsr, dest); barrier(gpu, game_motion.Get(), npsr, dest); }
        auto u0 = upload(gpu, game_color.Get(), color, 8), u1 = upload(gpu, game_depth.Get(), depth, 4), u2 = upload(gpu, game_motion.Get(), motion, 4);
        barrier(gpu, game_color.Get(), dest, source); barrier(gpu, game_depth.Get(), dest, source); barrier(gpu, game_motion.Get(), dest, source);
        // Copies common to both layouts (SR input copies). Previous-frame state is COPY_DEST for every target.
        for (auto* target : {sr_color_a.Get(), sr_depth_a.Get(), sr_motion_a.Get(), sr_color_b.Get(), sr_depth_b.Get(), sr_motion_b.Get()})
            if (frame) barrier(gpu, target, npsr, dest);
        copy(gpu, sr_color_a.Get(), game_color.Get()); copy(gpu, sr_depth_a.Get(), game_depth.Get()); copy(gpu, sr_motion_a.Get(), game_motion.Get());
        copy(gpu, sr_color_b.Get(), game_color.Get()); copy(gpu, sr_depth_b.Get(), game_depth.Get()); copy(gpu, sr_motion_b.Get(), game_motion.Get());
        // The extra copies of the separate layout (timed): NR colour, NR depth, NR motion.
        if (frame) { barrier(gpu, nr_color.Get(), npsr, dest); barrier(gpu, nr_depth.Get(), npsr, dest); barrier(gpu, nr_motion.Get(), npsr, dest); }
        gpu.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        copy(gpu, nr_color.Get(), game_color.Get()); copy(gpu, nr_depth.Get(), game_depth.Get()); copy(gpu, nr_motion.Get(), game_motion.Get());
        gpu.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        for (auto* target : {sr_color_a.Get(), sr_depth_a.Get(), sr_motion_a.Get(), sr_color_b.Get(), sr_depth_b.Get(), sr_motion_b.Get(),
                nr_color.Get(), nr_depth.Get(), nr_motion.Get()}) barrier(gpu, target, dest, npsr);
        barrier(gpu, game_color.Get(), source, npsr); barrier(gpu, game_depth.Get(), source, npsr); barrier(gpu, game_motion.Get(), source, npsr);
        const auto make_frame = [&](DlssViewId view, ID3D12Resource* c, ID3D12Resource* z, ID3D12Resource* m) {
            DlssNrFrame f{view, DlssNrRoute::d3d11_transport, gpu.list.Get(), c, uav, z, m,
                render_w, render_h, out_w, out_h, 0U, 0U, render_w, render_h, 0U, 0U, render_w, render_h,
                1.0f / render_w, 1.0f / render_h, true, frame == 0, 0x2BU};
            f.jitter_uv_x = (0.25f - 0.5f * (frame % 2)) / render_w; f.jitter_uv_y = (0.125f * (frame % 4) - 0.2f) / render_h;
            f.processing_width = render_w; f.processing_height = render_h;
            return f;
        };
        // Separate layout: NR in place on the NR colour, then the copy back (timed).
        barrier(gpu, nr_color.Get(), npsr, uav);
        gpu.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
        auto separate = make_frame(100, nr_color.Get(), nr_depth.Get(), nr_motion.Get());
        require(evaluate_dlss_nr(separate, settings), "Separate-layout NR evaluation failed");
        gpu.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);
        barrier(gpu, nr_color.Get(), uav, source); barrier(gpu, sr_color_a.Get(), npsr, dest);
        gpu.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 4);
        copy(gpu, sr_color_a.Get(), nr_color.Get());
        gpu.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 5);
        barrier(gpu, sr_color_a.Get(), dest, npsr); barrier(gpu, nr_color.Get(), source, npsr);
        // In-place layout: NR on the SR colour with the SR depth/motion copies.
        barrier(gpu, sr_color_b.Get(), npsr, uav);
        gpu.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 6);
        auto in_place = make_frame(101, sr_color_b.Get(), sr_depth_b.Get(), sr_motion_b.Get());
        require(evaluate_dlss_nr(in_place, settings), "In-place NR evaluation failed");
        gpu.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 7);
        barrier(gpu, sr_color_b.Get(), uav, npsr);
        gpu.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 8, times.Get(), 0);
        gpu.submit();
        UINT64* t{}; check(times->Map(0, nullptr, reinterpret_cast<void**>(&t)));
        removed_copies.push_back(ms(t[0], t[1], frequency) + ms(t[4], t[5], frequency));
        separate_nr.push_back(ms(t[2], t[3], frequency)); in_place_nr.push_back(ms(t[6], t[7], frequency));
        times->Unmap(0, nullptr);
        const auto a = download(gpu, sr_color_a.Get(), npsr, 8), b = download(gpu, sr_color_b.Get(), npsr, 8);
        const auto original = color;
        size_t differing{}, changed{};
        for (size_t i = 0; i < a.size(); i += 2) {
            differing += memcmp(a.data() + i, b.data() + i, 2) != 0;
            changed += memcmp(a.data() + i, original.data() + i, 2) != 0;
        }
        std::cout << "NR in place frame " << frame << " differing_halfs=" << differing << " nr_changed_halfs=" << changed
                  << " removed_copies_ms=" << removed_copies.back() << " nr_ms separate=" << separate_nr.back()
                  << " in_place=" << in_place_nr.back() << '\n';
        require(changed > a.size() / 20, "NR did not change the SR input");
        require(differing == 0, "In-place NR output differs from the separate-texture layout");
        if (dump) dump_file.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    }
    const auto average = [](const std::vector<double>& v) { return std::accumulate(v.begin() + 1, v.end(), 0.0) / double(v.size() - 1); };
    const auto bytes = [&](ID3D12Resource* r) { auto d = r->GetDesc(); return gpu.device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes; };
    const auto removed_bytes = bytes(nr_color.Get()) + bytes(nr_depth.Get()) + bytes(nr_motion.Get());
    std::cout << std::fixed << std::setprecision(3)
              << "NR in place per focus view: removed textures " << double(removed_bytes) / (1024.0 * 1024.0) << " MiB (colour "
              << double(bytes(nr_color.Get())) / (1024.0 * 1024.0) << ", depth " << double(bytes(nr_depth.Get())) / (1024.0 * 1024.0)
              << ", motion " << double(bytes(nr_motion.Get())) / (1024.0 * 1024.0) << "), removed copy GPU time "
              << average(removed_copies) << " ms (D3D12 copies; DX11 depth conversion excluded)\n";
    std::cout << "PASS: real DLSS-NR before upscaling in place on the SR input is bit-identical to the separate-texture layout ("
              << frames << " frames, 1204x1188 HDR)\n";
}
} // namespace nr_inplace
