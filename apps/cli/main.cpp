#include "formats/project/project.hpp"
#include "apps/cli/experiments.hpp"
#include "formats/snapshot/snapshot.hpp"
#include "results/csv.hpp"
#include <charconv>
#include <fstream>
#include <iostream>
#include <optional>
int main(int argc, char **argv) {
    const auto usage = [] {
        std::cerr << "Usage: powerdrive-cli project.pds [output.csv] [--snapshot-in state.pdss] "
                     "[--snapshot-out state.pdss] [--steps count] [--experiment name-or-UUID]\n";
        return 2;
    };
    if (argc < 2)
        return usage();
    std::string csv, state_in, state_out, experiment;
    size_t steps = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--experiment") {
            if (++i >= argc || !experiment.empty()) return usage();
            experiment = argv[i];
        } else if (option == "--snapshot-in" || option == "--snapshot-out") {
            if (++i >= argc)
                return usage();
            auto &path = option == "--snapshot-in" ? state_in : state_out;
            if (!path.empty())
                return usage();
            path = argv[i];
        } else if (option == "--steps") {
            if (++i >= argc || steps)
                return usage();
            const std::string value = argv[i];
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), steps);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !steps)
                return usage();
        } else if (!option.empty() && option.front() != '-' && csv.empty())
            csv = option;
        else
            return usage();
    }
    try {
        std::ifstream input(argv[1]);
        if (!input)
            throw pds::Diagnostic("read_error", argv[1], "Cannot open project");
        auto project = pds::read_project(input);
        if (!experiment.empty()) {
            if (!state_in.empty() || !state_out.empty() || steps) return usage();
            return run_cli_experiment(project, experiment, csv);
        }
        std::optional<pds::SimulationSnapshot> snapshot;
        if (!state_in.empty()) {
            std::ifstream file(state_in);
            if (!file)
                throw pds::Diagnostic("read_error", state_in, "Cannot open state file");
            snapshot = pds::read_snapshot(file);
        }
        pds::ExecutionOptions options{snapshot ? &*snapshot : nullptr, !state_out.empty(), steps};
        const auto result =
            pds::execute(pds::compile(project), nullptr, nullptr, nullptr, nullptr, {}, &options);
        if (!csv.empty()) {
            std::ofstream out(csv);
            pds::write_csv(result, out);
            out.close();
            if (!out)
                throw pds::Diagnostic("write_error", csv, "Cannot write results");
        }
        if (!state_out.empty()) {
            std::ofstream out(state_out);
            pds::write_snapshot(*result.snapshot, out);
            out.close();
            if (!out)
                throw pds::Diagnostic("write_error", state_out, "Cannot write state file");
        }
        std::cout << "Reference CPU | float64 | " << pds::method_name(result.profile.method)
                  << " | steps=" << result.accepted_steps << " | linear_solves=" << result.linear_solves
                  << " | samples=" << result.samples.size() << " | residual=" << result.max_scaled_residual
                  << " | time=" << result.last_time << '\n';
        if (result.profile.step_control.adaptive)
            std::cout << "Adaptive | rejected=" << result.rejected_steps << " | min_step=" << result.min_accepted_step
                      << " | max_step=" << result.max_accepted_step << " | max_error_ratio=" << result.max_local_error
                      << " | reduction=" << result.step_reduction_reason << '\n';
        return 0;
    } catch (const pds::Diagnostic &e) {
        std::cerr << e.code << " object=" << e.object << " time=" << e.time << ": " << e.what() << '\n';
        return 1;
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
