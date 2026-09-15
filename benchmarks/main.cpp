#include "formats/project/project.hpp"
#include "core/model/connectivity.hpp"
#include "core/solver/reference/reference.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
int main(int argc,char** argv) {
    if(argc<2 || argc>4) { std::cerr<<"Usage: powerdrive-benchmark project.pds [repeats] [none|stream]\n"; return 2; }
    try {
        int repeats=argc>=3?std::stoi(argv[2]):10;
        if(repeats<1 || repeats>10000) throw std::runtime_error("Repeats must be 1..10000");
        std::ifstream in(argv[1]); auto p=pds::read_project(in);
        using Clock=std::chrono::steady_clock;
        const auto t0=Clock::now();
        auto ir=pds::compile(p);
        const auto t1=Clock::now();
        pds::Recording recording;
        const std::string mode=argc==4?argv[3]:"all";
        if(mode!="all"&&mode!="none"&&mode!="stream")throw std::runtime_error("Expected none or stream");
        recording.all=mode=="all";
        if(mode=="stream") {
            recording.channels=p.scope_enabled?p.scope_channels:std::vector<std::string>{};
            for(const auto& plot:p.plots)for(const auto& key:pds::plot_channels(p,plot.id))recording.channels.push_back(key);
        }
        size_t steps=0,stored=0,solves=0,samples=0; double residual=0,checksum=0,first_sample=0;
        for(int i=0;i<repeats;++i) {
            const auto start=Clock::now();
            double run_checksum=0;
            std::function<void(pds::Result&&)> stream;
            if(mode=="stream") stream=[&](pds::Result&& batch){
                if(samples==0)first_sample=std::chrono::duration<double>(Clock::now()-start).count();
                samples+=batch.samples.size();
                run_checksum=0;for(double v:batch.samples.back().values)run_checksum+=v;
            };
            auto r=pds::execute(ir,nullptr,nullptr,&recording,nullptr,stream); steps+=r.accepted_steps; solves+=r.linear_solves;
            samples+=r.samples.size();
            stored=r.samples.size()*(sizeof(pds::Sample)+ir.unknowns.size()*sizeof(double));
            residual=std::max(residual,r.max_scaled_residual);
            if(!r.samples.empty()){run_checksum=0;for(double v:r.samples.back().values) run_checksum+=v;}
            checksum+=run_checksum;
        }
        const double elapsed=std::chrono::duration<double>(Clock::now()-t1).count();
        std::cout<<"backend=Reference CPU precision=float64 compiler="<<
#ifdef _MSC_VER
            "MSVC "<<_MSC_VER
#else
            __VERSION__
#endif
            <<"\ncompile_seconds="<<std::chrono::duration<double>(t1-t0).count()
            <<"\nmode="<<mode<<"\nfirst_sample_seconds="<<first_sample<<"\nsamples="<<samples
            <<"\nwall_seconds="<<elapsed<<"\nsteps="<<steps<<"\nsteps_per_second="<<steps/elapsed
            <<"\nwall_per_simulated_second="<<elapsed/(p.profile.stop*repeats)
            <<"\nresult_payload_estimate_bytes="<<stored
            <<"\nlinear_solves="<<solves<<"\nmax_scaled_residual="<<residual<<"\nchecksum="<<checksum<<'\n';
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
