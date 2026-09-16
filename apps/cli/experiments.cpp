#include "apps/cli/experiments.hpp"
#include "core/experiment/sweep.hpp"
#include "results/experiment_csv.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>

int run_cli_experiment(const pds::Project &project, const std::string &selector,
                       const std::string &csv_path) {
    const pds::Experiment *selected = nullptr;
    for (const auto &e : project.experiments)
        if (e.id == selector || e.name == selector) {
            if (selected)
                throw pds::Diagnostic("ambiguous_experiment", selector, "Use an experiment UUID");
            selected = &e;
        }
    if (!selected)
        throw pds::Diagnostic("missing_experiment", selector, "Experiment does not exist");
    auto experiment = *selected;
    experiment.retain_curves = false; // CLI summary does not accumulate individual waveforms.
    std::ofstream csv;
    if (!csv_path.empty()) {
        csv.open(csv_path);
        if (!csv)
            throw pds::Diagnostic("write_error", csv_path, "Cannot open experiment summary");
        pds::write_experiment_csv_header(csv);
    }
    const auto progress = pds::run_experiment(project, experiment, nullptr, [&](pds::ExperimentCase &&c) {
        if (csv.is_open())
            pds::write_experiment_csv_case(csv, c);
        if (!c.error_code.empty())
            std::cerr << "Case " << c.index + 1 << ": " << c.error_code << " object=" << c.error_object << " "
                      << c.error_message << '\n';
    });
    if (csv.is_open()) {
        csv.close();
        if (!csv)
            throw pds::Diagnostic("write_error", csv_path, "Cannot finish experiment summary");
    }
    std::cout << "Experiment " << experiment.name << " | completed=" << progress.completed << '/'
              << progress.total << " | failed=" << progress.failed << '\n';
    return progress.failed || progress.cancelled ? 1 : 0;
}
