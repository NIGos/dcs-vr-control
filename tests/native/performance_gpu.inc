// Hardware timestamps cover real production GPU work. Upload/readback, fixture
// waits and initialization are excluded; these numbers are not DCS frame times.
void run_performance_gpu(Gpu& gpu, const std::array<ComPtr<ID3D12Resource>,2>& focus,
    const std::filesystem::path& workspace, Settings settings, ID3D12Resource* depth,
    ID3D12Resource* motion, float neuralScale, std::wstring_view flowArgument,
    const std::filesystem::path& reportPath) {
    require(neuralScale>=.1f && neuralScale<=1,"Invalid neural scale");
    const auto comma=flowArgument.find(L','); require(comma!=flowArgument.npos,"Expected flowScale,preset");
    auto scale=std::stoi(std::wstring(flowArgument.substr(0,comma)));
    const auto nextComma=flowArgument.find(L',',comma+1);
    auto preset=flowArgument.substr(comma+1,nextComma==flowArgument.npos?flowArgument.npos:nextComma-comma-1);
    bool bidirectional=false;
    if(nextComma!=flowArgument.npos) { auto flag=flowArgument.substr(nextComma+1); require(flag==L"0" || flag==L"1","Invalid bidirectional flag"); bidirectional=flag==L"1"; }
    require(scale==50 || scale==75 || scale==100,"Invalid flow scale");
    require(preset==L"fast" || preset==L"medium" || preset==L"slow","Invalid flow preset");
    settings.nr_working_scale=neuralScale; settings.nr_processing_order=NrProcessingOrder::before_upscaling;
    QuadCompositor quad(gpu,workspace,focus); auto composed=quad.output(),current=quad.output(),synthetic=quad.output();
    std::array<ID3D12Resource*,1> sources{composed.Get()},currents{current.Get()},synthetics{synthetic.Get()};
    auto history=std::make_shared<xrfg::D3D12SwapchainHistory>(); check(history->initialize(gpu.device.Get(),gpu.queue.Get(),sources,D3D12_RESOURCE_STATE_RENDER_TARGET));
    xrfg::D3D12NvidiaOpticalFlowOptions options;
    options.bidirectional=bidirectional;
    options.input_scale=scale==50?xrfg::D3D12NvidiaInputScale::half:scale==75?xrfg::D3D12NvidiaInputScale::three_quarter:xrfg::D3D12NvidiaInputScale::full;
    options.preset=preset==L"fast"?xrfg::D3D12NvidiaPerformancePreset::fast:preset==L"slow"?xrfg::D3D12NvidiaPerformancePreset::slow:xrfg::D3D12NvidiaPerformancePreset::medium;
    xrfg::D3D12FrameSynthesizer fg; check(fg.initialize(gpu.device.Get(),gpu.queue.Get(),history,currents,synthetics,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_RENDER_TARGET,xrfg::D3D12OpticalFlowBackend::nvidia,options,true));
    std::array<xrfg::D3D12ReprojectionView,2> views{};
    for(unsigned eye=0;eye<2;++eye) { views[eye].pose.orientation.w=1; views[eye].pose.position.x=eye==0?-.032f:.032f; views[eye].fov={-.75f,.75f,.6f,-.6f}; views[eye].image_rect={0,0,stereo_extent,stereo_extent}; views[eye].array_slice=eye; }
    ComPtr<ID3D12QueryHeap> queries; D3D12_QUERY_HEAP_DESC q{}; q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP; q.Count=4; check(gpu.device->CreateQueryHeap(&q,IID_PPV_ARGS(&queries)));
    auto times=gpu.buffer(4*sizeof(UINT64),D3D12_HEAP_TYPE_READBACK); UINT64 frequency{}; check(gpu.queue->GetTimestampFrequency(&frequency)); require(frequency>0,"No GPU timestamp frequency");
    std::vector<double> nr, composition, opticalFlow, gpuSum, cpuSubmit;
    constexpr unsigned warmup=6,samples=24;
    auto firstResourceCount=std::uint64_t{};
    auto begin=std::chrono::steady_clock::now();
    for(unsigned frame=0;frame<warmup+samples;++frame) {
        auto cpuBegin=std::chrono::steady_clock::now();
        gpu.list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        for(unsigned eye=0;eye<2;++eye) {
            DlssNrFrame f{}; f.view_id=2+eye; f.route=DlssNrRoute::d3d12_native; f.command_list=gpu.list.Get(); f.color=focus[eye].Get(); f.depth=depth; f.motion_vectors=motion; f.input_width=f.output_width=f.depth_width=f.motion_width=width; f.input_height=f.output_height=f.depth_height=f.motion_height=height; f.depth_state=f.motion_state=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            require(evaluate_dlss_nr(f,settings),"Benchmark neural evaluation failed");
        }
        gpu.list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        gpu.list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,2);
        quad.compose(composed.Get(),focus,false);
        gpu.list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,3);
        gpu.list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,4,times.Get(),0);
        auto cpuEnd=std::chrono::steady_clock::now(); gpu.submit();
        xrfg::D3D12HistoryCaptureTicket capture{}; check(history->capture(0,&capture)); check(history->commit(capture)); xrfg::D3D12FrameSynthesisTicket ticket{};
        if(frame==0) check(fg.submit_prime(capture,views,0,&ticket)); else check(fg.submit_pair(capture,views,views,0,0,&ticket));
        check(fg.wait_for_previous_submission(60000));
        auto snapshot=dlss_nr_snapshot(); require(snapshot.failed_calls==0,"Benchmark had a neural fallback");
        if(frame==warmup-1) firstResourceCount=snapshot.codec_creations;
        xrfg::D3D12NvidiaGpuTiming timing{};
        if(frame>0) { require(fg.consume_nvidia_gpu_timing(&timing)==S_OK && timing.eye_count==2,"Missing stereo optical-flow timing"); }
        if(frame<warmup) continue;
        UINT64* mapped{}; check(times->Map(0,nullptr,reinterpret_cast<void**>(&mapped))); auto neural=double(mapped[1]-mapped[0])*1e6/frequency, composite=double(mapped[3]-mapped[2])*1e6/frequency; times->Unmap(0,nullptr);
        nr.push_back(neural); composition.push_back(composite); opticalFlow.push_back(double(timing.total_microseconds)); gpuSum.push_back(neural+composite+timing.total_microseconds); cpuSubmit.push_back(std::chrono::duration<double,std::micro>(cpuEnd-cpuBegin).count());
        require(snapshot.codec_creations==firstResourceCount,"Steady-state neural textures were recreated");
    }
    auto expected=quad.read(composed.Get()), actual=quad.read(current.Get()); require(actual==expected,"Benchmark real frame changed");
    auto output=gpu.read(focus[0].Get()); for(float component:output) require(std::isfinite(component),"Non-finite benchmark output");
    auto snapshot=dlss_nr_snapshot(); require(snapshot.evaluation_calls==2*(warmup+samples),"Missing neural benchmark evaluations");
    check(fg.retire_previous()); check(fg.wait_for_idle());
    auto quantile=[](std::vector<double> v,double p) { std::sort(v.begin(),v.end()); return v[std::min(v.size()-1,size_t(std::ceil(p*v.size())-1))]; };
    auto stats=[&](std::ostream& out,const std::vector<double>& v) { out<<"{\"medianUs\":"<<quantile(v,.5)<<",\"p95Us\":"<<quantile(v,.95)<<",\"minUs\":"<<*std::min_element(v.begin(),v.end())<<'}'; };
    std::ofstream report(reportPath); require(bool(report),"Cannot write benchmark report"); report<<std::fixed<<std::setprecision(3);
    const char* presetName=preset==L"fast"?"fast":preset==L"slow"?"slow":"medium";
    report<<"{\"passed\":true,\"focusDimension\":"<<width<<",\"stereoDimension\":"<<stereo_extent<<",\"neuralScale\":"<<neuralScale<<",\"flowScale\":"<<scale<<",\"flowPreset\":\""<<presetName<<"\",\"bidirectional\":"<<(bidirectional?"true":"false")<<",\"samples\":"<<samples<<",\"warmupFrames\":"<<warmup<<",\"neuralEvaluations\":"<<snapshot.evaluation_calls<<",\"neuralFailures\":"<<snapshot.failed_calls<<",\"codecCreations\":"<<snapshot.codec_creations<<",\"steadyStateRecreations\":0,\"neuralIntermediateBytesPerView\":"<<snapshot.intermediate_vram_bytes<<",\"wallSeconds\":"<<std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count()<<",\"neural\":"; stats(report,nr); report<<",\"composition\":"; stats(report,composition); report<<",\"opticalFlow\":"; stats(report,opticalFlow); report<<",\"gpuStageSum\":"; stats(report,gpuSum); report<<",\"cpuRecording\":"; stats(report,cpuSubmit); report<<"}\n";
    std::cout<<"PASS: measured "<<samples<<" warmed frames, both neural histories, no steady-state texture recreation\n";
}
