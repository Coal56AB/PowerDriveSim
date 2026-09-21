#pragma once
#include "core/ir/ir.hpp"
#include "core/ir/snapshot.hpp"
#include "core/solver/reference/block_vector.hpp"
#include <atomic>
#include <map>
#include <functional>
#include <optional>
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
    std::vector<std::string> gate_names;
    BlockVector<Sample> samples;
    size_t accepted_steps=0, linear_solves=0, max_step_iterations=0;
    size_t rejected_steps=0;
    double min_accepted_step=0, max_accepted_step=0, max_local_error=0;
    std::string step_reduction_reason;
    bool cancelled=false;
    double max_scaled_residual=0.0, last_time=0.0;
    std::optional<SimulationSnapshot> snapshot;
};
struct ExecutionOptions {
    const SimulationSnapshot* resume = nullptr;
    bool capture_snapshot = false;
    size_t max_steps = 0; // Zero means no per-call limit; useful for stepping/checkpoints.
    size_t stream_preview_samples = 0; // Zero preserves every streamed sample; desktop may request a bounded preview.
};
struct Recording { bool all=true; std::vector<std::string> channels; };
std::vector<Channel> available_channels(const SimulationIR& ir);
Result select_result(const Result& result,const std::vector<std::string>& channels);
void accumulate_statistics(Result &result, const Result &previous);
Result execute(const SimulationIR& ir, const std::atomic_bool* cancel=nullptr,
               std::atomic<double>* simulated_time=nullptr,const Recording* recording=nullptr,
               const std::atomic_bool* paused=nullptr,
               const std::function<void(Result&&)>& stream={},
               const ExecutionOptions* options=nullptr);
}
