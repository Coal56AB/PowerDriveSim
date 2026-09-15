#include "formats/project/project.hpp"
#include "core/solver/reference/reference.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
int main(int argc,char** argv) {
    if(argc<2 || argc>3) { std::cerr<<"Usage: powerdrive-benchmark project.pds [repeats]\n"; return 2; }
    try {
        int repeats=argc==3?std::stoi(argv[2]):10;
        if(repeats<1 || repeats>10000) throw std::runtime_error("Repeats must be 1..10000");
        std::ifstream in(argv[1]); auto p=pds::read_project(in);
        using Clock=std::chrono::steady_clock;
        const auto t0=Clock::now();
        auto ir=pds::compile(p);
        const auto t1=Clock::now();
        size_t steps=0,stored=0; double residual=0,checksum=0;
        for(int i=0;i<repeats;++i) {
            auto r=pds::execute(ir); steps+=r.accepted_steps;
            stored=r.samples.size()*(sizeof(pds::Sample)+ir.unknowns.size()*sizeof(double));
            residual=std::max(residual,r.max_scaled_residual);
            for(double v:r.samples.back().values) checksum+=v;
        }
        const double elapsed=std::chrono::duration<double>(Clock::now()-t1).count();
        std::cout<<"backend=Reference CPU precision=float64 compiler="<<
#ifdef _MSC_VER
            "MSVC "<<_MSC_VER
#else
            __VERSION__
#endif
            <<"\ncompile_seconds="<<std::chrono::duration<double>(t1-t0).count()
            <<"\nwall_seconds="<<elapsed<<"\nsteps="<<steps<<"\nsteps_per_second="<<steps/elapsed
            <<"\nwall_per_simulated_second="<<elapsed/(p.profile.stop*repeats)
            <<"\nresult_payload_estimate_bytes="<<stored
            <<"\nmax_scaled_residual="<<residual<<"\nchecksum="<<checksum<<'\n';
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}