#pragma once
#include "core/ir/ir.hpp"
#include <atomic>
#include <map>
namespace pds {
class StampSystem {
public:
    std::vector<std::map<int,double>> rows;
    std::vector<double> rhs;
    explicit StampSystem(size_t n):rows(n),rhs(n,0.0){}
    void add(int row,int column,double value);
    void inject(int row,double value);
    void incidence(int p,int n,int branch);
    void conductance(int p,int n,double value);
    std::vector<double> solve(const SimulationIR& ir,double time) const;
};
struct Sample { double time; std::vector<double> values; std::vector<bool> gates; };
struct Result {
    std::string project_id, backend="Reference CPU", precision="float64", engine="0.4.0";
    Profile profile;
    std::vector<Channel> channels;
    std::vector<std::string> gate_objects;
    std::vector<Sample> samples;
    size_t accepted_steps=0, linear_solves=0, max_step_iterations=0;
    bool cancelled=false;
    double max_scaled_residual=0.0, last_time=0.0;
};
struct Recording { bool all=true; std::vector<std::string> channels; };
std::vector<Channel> available_channels(const SimulationIR& ir);
Result select_result(const Result& result,const std::vector<std::string>& channels);
Result execute(const SimulationIR& ir, const std::atomic_bool* cancel=nullptr,
               std::atomic<double>* simulated_time=nullptr,const Recording* recording=nullptr);
}