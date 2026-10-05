// Included after the offscreen Gpu helper. Uses the exact Quad Views shaders and
// source-built OFXR synthesis implementation, without pretending to test an XR session.
struct QuadCompositor {
    Gpu& gpu;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> srvs, rtvs;
    UINT srvStep{}, rtvStep{};
    std::array<ComPtr<ID3D12Resource>,2> peripheral;
    QuadCompositor(Gpu& g, const std::filesystem::path& workspace,
                   const std::array<ComPtr<ID3D12Resource>,2>& focus) : gpu(g) {
        D3D12_DESCRIPTOR_RANGE range{}; range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors=2;
        std::array<D3D12_ROOT_PARAMETER,3> params{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[0].Constants={0,0,16}; params[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[1].Constants={0,0,4}; params[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[2].DescriptorTable={1,&range}; params[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR; sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP; sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS; sampler.MaxLOD=D3D12_FLOAT32_MAX; sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rootDesc{}; rootDesc.NumParameters=3; rootDesc.pParameters=params.data(); rootDesc.NumStaticSamplers=1; rootDesc.pStaticSamplers=&sampler;
        ComPtr<ID3DBlob> serialized, error; check(D3D12SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error));
        check(gpu.device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)));
        ComPtr<ID3DBlob> vs,ps;
        auto shader=workspace/L"external/quadviews/openxr-api-layer";
        check(D3DCompileFromFile((shader/L"ProjectionVS.hlsl").c_str(),nullptr,nullptr,"main","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error));
        check(D3DCompileFromFile((shader/L"ProjectionPS.hlsl").c_str(),nullptr,nullptr,"main","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{}; p.pRootSignature=root.Get(); p.VS={vs->GetBufferPointer(),vs->GetBufferSize()}; p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        p.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL; p.SampleMask=UINT_MAX; p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE; p.RasterizerState.DepthClipEnable=TRUE;
        p.DepthStencilState.DepthEnable=FALSE; p.DepthStencilState.StencilEnable=FALSE; p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; p.NumRenderTargets=1; p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM; p.SampleDesc.Count=1;
        check(gpu.device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&pipeline)));
        D3D12_DESCRIPTOR_HEAP_DESC h{}; h.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; h.NumDescriptors=4; h.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; check(gpu.device->CreateDescriptorHeap(&h,IID_PPV_ARGS(&srvs)));
        h.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; h.NumDescriptors=2; h.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE; check(gpu.device->CreateDescriptorHeap(&h,IID_PPV_ARGS(&rtvs)));
        srvStep=gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); rtvStep=gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for(unsigned eye=0;eye<2;++eye) {
            // A genuinely lower-resolution peripheral image (256 vs 512 pixels).
            std::vector<float> pixels(width*height*4);
            for(size_t i=0;i<pixels.size();i+=4) { pixels[i]=.08f+eye*.14f; pixels[i+1]=.12f; pixels[i+2]=.17f; pixels[i+3]=1; }
            auto full=gpu.texture(DXGI_FORMAT_R32G32B32A32_FLOAT,pixels);
            D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT; auto d=full->GetDesc(); d.Width=width/2; d.Height=height/2;
            check(gpu.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&peripheral[eye])));
            D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition={full.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE}; gpu.list->ResourceBarrier(1,&b);
            D3D12_TEXTURE_COPY_LOCATION dst{peripheral[eye].Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, src{full.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; D3D12_BOX box{0,0,0,width/2,height/2,1}; gpu.list->CopyTextureRegion(&dst,0,0,0,&src,&box);
            b.Transition={peripheral[eye].Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}; gpu.list->ResourceBarrier(1,&b); gpu.submit();
            D3D12_SHADER_RESOURCE_VIEW_DESC s{}; s.Format=d.Format; s.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; s.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; s.Texture2D.MipLevels=1;
            auto handle=srvs->GetCPUDescriptorHandleForHeapStart(); handle.ptr+=eye*2*srvStep; gpu.device->CreateShaderResourceView(peripheral[eye].Get(),&s,handle); handle.ptr+=srvStep; gpu.device->CreateShaderResourceView(focus[eye].Get(),&s,handle);
        }
    }
    ComPtr<ID3D12Resource> output() {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT; D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=stereo_extent; d.Height=stereo_extent; d.DepthOrArraySize=2; d.MipLevels=1; d.Format=DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count=1; d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> r; check(gpu.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&r))); return r;
    }
    void compose(ID3D12Resource* target, const std::array<ComPtr<ID3D12Resource>,2>& focus, bool submit = true) {
        for(unsigned eye=0;eye<2;++eye) {
            D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition={focus[eye].Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}; gpu.list->ResourceBarrier(1,&b);
            D3D12_RENDER_TARGET_VIEW_DESC d{}; d.Format=DXGI_FORMAT_R8G8B8A8_UNORM; d.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DARRAY; d.Texture2DArray.ArraySize=1; d.Texture2DArray.FirstArraySlice=eye;
            auto rtv=rtvs->GetCPUDescriptorHandleForHeapStart(); rtv.ptr+=eye*rtvStep; gpu.device->CreateRenderTargetView(target,&d,rtv);
            gpu.list->SetGraphicsRootSignature(root.Get()); gpu.list->SetPipelineState(pipeline.Get()); ID3D12DescriptorHeap* heaps[]{srvs.Get()}; gpu.list->SetDescriptorHeaps(1,heaps);
            const float projection[]{2,0,0,0,0,2,0,0,0,0,1,0,0,0,0,1}; const UINT ps[]{0,1,1,0};
            gpu.list->SetGraphicsRoot32BitConstants(0,16,projection,0); gpu.list->SetGraphicsRoot32BitConstants(1,4,ps,0); auto srv=srvs->GetGPUDescriptorHandleForHeapStart(); srv.ptr+=eye*2*srvStep; gpu.list->SetGraphicsRootDescriptorTable(2,srv);
            D3D12_VIEWPORT viewport{0,0,float(stereo_extent),float(stereo_extent),0,1}; D3D12_RECT scissor{0,0,LONG(stereo_extent),LONG(stereo_extent)}; gpu.list->RSSetViewports(1,&viewport); gpu.list->RSSetScissorRects(1,&scissor); gpu.list->OMSetRenderTargets(1,&rtv,FALSE,nullptr); gpu.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); gpu.list->DrawInstanced(3,1,0,0);
            std::swap(b.Transition.StateBefore,b.Transition.StateAfter); gpu.list->ResourceBarrier(1,&b);
        }
        if (submit) gpu.submit();
    }
    std::array<std::vector<unsigned char>,2> read(ID3D12Resource* target) {
        auto d=target->GetDesc(); std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,2> fp{}; UINT64 size{}; gpu.device->GetCopyableFootprints(&d,0,2,0,fp.data(),nullptr,nullptr,&size); auto buf=gpu.buffer(size,D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition={target,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE}; gpu.list->ResourceBarrier(1,&b);
        for(unsigned eye=0;eye<2;++eye) { D3D12_TEXTURE_COPY_LOCATION dst{buf.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; dst.PlacedFootprint=fp[eye]; D3D12_TEXTURE_COPY_LOCATION src{target,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; src.SubresourceIndex=eye; gpu.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr); }
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter); gpu.list->ResourceBarrier(1,&b); gpu.submit(); void* data{}; check(buf->Map(0,nullptr,&data)); std::array<std::vector<unsigned char>,2> pixels;
        for(unsigned eye=0;eye<2;++eye) { pixels[eye].resize(stereo_extent*stereo_extent*4); for(UINT y=0;y<stereo_extent;++y) memcpy(pixels[eye].data()+y*stereo_extent*4,static_cast<char*>(data)+fp[eye].Offset+y*fp[eye].Footprint.RowPitch,stereo_extent*4); } buf->Unmap(0,nullptr); return pixels;
    }
};

void verify_combined_gpu(Gpu& gpu, const std::array<ComPtr<ID3D12Resource>,2>& focus,
    const std::filesystem::path& workspace, Settings settings, ID3D12Resource* depth, ID3D12Resource* motion) {
    QuadCompositor quad(gpu,workspace,focus); auto composed=quad.output(), current=quad.output(), synthetic=quad.output();
    std::array<ID3D12Resource*,1> sources{composed.Get()}, currents{current.Get()}, synthetics{synthetic.Get()};
    auto history=std::make_shared<xrfg::D3D12SwapchainHistory>(); check(history->initialize(gpu.device.Get(),gpu.queue.Get(),sources,D3D12_RESOURCE_STATE_RENDER_TARGET));
    xrfg::D3D12FrameSynthesizer fg; check(fg.initialize(gpu.device.Get(),gpu.queue.Get(),history,currents,synthetics,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_RENDER_TARGET,xrfg::D3D12OpticalFlowBackend::nvidia,{},true));
    std::array<xrfg::D3D12ReprojectionView,2> views{};
    for(unsigned eye=0;eye<2;++eye) { views[eye].pose.orientation.w=1; views[eye].pose.position.x=eye==0?-.032f:.032f; views[eye].fov={-.75f,.75f,.6f,-.6f}; views[eye].image_rect={0,0,width,height}; views[eye].array_slice=eye; }
    for(unsigned frame=0;frame<3;++frame) {
        // Retune through the actual model's creation-time API; no fake NGX evaluator.
        settings.nr_local_tone_strength=1.1f; settings.nr_local_structure_strength=.9f; settings.nr_skin_structure_strength=1.2f;
        settings.nr_processing_order=NrProcessingOrder::before_upscaling;
        settings.nr_automatic_mask=true; settings.nr_ui_correction=true;
        for(unsigned eye=0;eye<2;++eye) {
            DlssNrFrame f{}; f.view_id=2+eye; f.route=DlssNrRoute::d3d12_native; f.command_list=gpu.list.Get(); f.color=focus[eye].Get(); f.depth=depth; f.motion_vectors=motion; f.input_width=f.output_width=f.depth_width=f.motion_width=width; f.input_height=f.output_height=f.depth_height=f.motion_height=height; f.depth_state=f.motion_state=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            require(evaluate_dlss_nr(f,settings),"Combined frame neural execution failed"); gpu.submit();
        }
        quad.compose(composed.Get(),focus); auto expected=quad.read(composed.Get());
        for(unsigned eye=0;eye<2;++eye) {
            auto corner=(8*width+8)*4, center=((height/2)*width+width/2)*4;
            require(std::abs(int(expected[eye][corner])-int((.08f+eye*.14f)*255))<=1,"Peripheral colour did not survive foveated composition");
            require(expected[eye][center]!=expected[eye][corner],"Focus image absent from composition");
        }
        xrfg::D3D12HistoryCaptureTicket capture{}; check(history->capture(0,&capture)); check(history->commit(capture)); xrfg::D3D12FrameSynthesisTicket ticket{};
        if(frame==0) check(fg.submit_prime(capture,views,0,&ticket));
        else check(fg.submit_pair(capture,views,views,0,0,&ticket));
        check(fg.wait_for_previous_submission(60000)); auto actual=quad.read(current.Get()); require(actual==expected,"FG changed the real composed frame");
        if(frame>0) {
            auto mid=quad.read(synthetic.Get()); size_t changed[2]{};
            for(unsigned eye=0;eye<2;++eye) for(size_t i=0;i<mid[eye].size();++i) changed[eye]+=mid[eye][i]!=actual[eye][i];
            std::cout<<"Combined frame "<<frame<<" synthetic_changed="<<changed[0]<<','<<changed[1]<<" serials="<<ticket.previous_serial<<','<<ticket.current_serial<<'\n';
            require(changed[0]>100 && changed[1]>100,"Synthetic stereo frames are passthrough");
            xrfg::D3D12NvidiaGpuTiming timing{}; check(fg.consume_nvidia_gpu_timing(&timing)); require(timing.eye_count==2,"Optical flow did not process both eyes");
            std::cout<<"NVIDIA optical flow GPU time: "<<timing.total_microseconds<<" us, two eyes\n";
        }
    }
    check(fg.retire_previous()); check(fg.wait_for_idle());
    std::cout<<"PASS: real DLSS-NR -> exact Quad Views composition shaders -> NVIDIA stereo optical-flow synthesis on one GPU\n";
}
