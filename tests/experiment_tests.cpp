#include "core/experiment/sweep.hpp"
#include "core/editor/document.hpp"
#include "core/model/hierarchy.hpp"
#include "formats/project/project.hpp"
#include "results/experiment_csv.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>
using namespace pds;
namespace {
std::string id(int value) {
    return derived_uuid("experiment-test:" + std::to_string(value));
}
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void near(double a, double b, double tolerance = 1e-9) {
    check(std::abs(a - b) < tolerance, "Sweep numeric mismatch");
}
Project rc() {
    Project p;
    p.id = id(0);
    p.name = "RC sweep";
    p.profile = {.005, 1e-6, Method::trapezoidal};
    p.nodes = {{id(1), "Ground", true}, {id(2), "Supply"}, {id(3), "Output"}};
    p.components = {{id(4), "V", Kind::voltage, id(2), id(1), 1},
                    {id(5), "R", Kind::resistor, id(2), id(3), 1000},
                    {id(6), "C", Kind::capacitor, id(3), id(1), 1e-6}};
    return p;
}
Project nested() {
    Project p;
    p.id = id(20);
    p.name = "Independent nested loads";
    p.wired = true;
    p.nodes = {{id(21), "Ground", true}};
    p.components = {{id(22), "V", Kind::voltage, "", "", 10}};
    Definition d;
    d.id = id(30);
    d.name = "R";
    d.wired = true;
    d.components = {{id(31), "R", Kind::resistor, "", "", 100}};
    d.ports = {{id(32), "p", {id(31), "p"}}, {id(33), "n", {id(31), "n"}}};
    d.parameters = {{id(34), "Resistance", "Ohm", id(31), "value", 100}};
    Definition wrapper;
    wrapper.id = id(40);
    wrapper.name = "Wrapper";
    wrapper.wired = true;
    wrapper.instances = {{id(41), "R", d.id}};
    wrapper.ports = {{id(32), "p", {id(41), id(32)}}, {id(33), "n", {id(41), id(33)}}};
    wrapper.parameters = {{id(34), "Resistance", "Ohm", id(41), id(34), 100}};
    p.definitions = {d, wrapper};
    p.instances = {{id(50), "First", wrapper.id}, {id(51), "Second", wrapper.id}};
    p.instances[1].parameters = {{id(34), 200}};
    auto wire = [&](Endpoint a, Endpoint b) { p.wires.push_back({new_uuid(), a, b, {}}); };
    wire({id(22), "n"}, {id(21), "node"});
    for (const auto &i : p.instances) {
        wire({i.id, id(32)}, {id(22), "p"});
        wire({i.id, id(33)}, {id(21), "node"});
    }
    return p;
}
} // namespace
int main() try {
    auto p = rc();
    const auto original = p;
    Experiment e;
    e.id = id(10);
    e.name = "R and V";
    e.retain_curves = true;
    e.axes = {{{{}, id(5), "value"}, {1000, 2000, -1}}};
    e.scenarios = {{"1 V", {{{{}, id(4), "value"}, 1}}}, {"2 V", {{{{}, id(4), "value"}, 2}}}};
    e.channels = {id(3)};
    p.experiments = {e};
    std::stringstream file;
    write_project(p, file);
    const auto restored = read_project(file);
    check(restored == p, "Experiment serialization");
    std::vector<ExperimentCase> cases;
    auto report = run_experiment(p, e, nullptr, [&](ExperimentCase &&c) { cases.push_back(std::move(c)); });
    check(report.total == 6 && report.completed == 6 && report.failed == 2 && !report.cancelled,
          "Fault sweep counts");
    for (size_t n = 0; n < 4; ++n) {
        const auto &c = cases[n];
        const double resistance = n < 2 ? 1000 : 2000, voltage = n % 2 ? 2 : 1;
        check(c.index == n && c.measurements.size() == 1 && c.curves && c.error_code.empty(),
              "Case metadata");
        near(c.measurements[0].final_value, voltage * (1 - std::exp(-.005 / (resistance * 1e-6))), 2e-7);
        check(c.measurements[0].time.intervals == 5000, "Time-weighted sweep metrics");
    }
    check(!cases[4].error_code.empty() && !cases[5].error_code.empty(), "Faults isolated per scenario");
    std::ostringstream summary;
    write_experiment_csv_header(summary);
    for (const auto &c : cases)
        write_experiment_csv_case(summary, c);
    check(summary.str().find("mean,rms,stddev,integral") != std::string::npos &&
              summary.str().find("failed") != std::string::npos,
          "Experiment summary export");
    size_t n = 0;
    run_experiment(p, e, nullptr, [&](ExperimentCase &&c) {
        if (c.curves) {
            const auto &before = cases[n].curves->samples;
            check(before.size() == c.curves->samples.size(), "Repeat sample count");
            for (size_t k = 0; k < before.size(); ++k)
                check(before[k].time == c.curves->samples[k].time &&
                          before[k].values == c.curves->samples[k].values,
                      "Repeated sweep is not bit-identical");
        }
        ++n;
    });
    p.experiments.clear();
    check(p == original, "Sweep mutated source project");
    std::atomic_bool cancel = false;
    auto stopped = run_experiment(p, e, &cancel, [&](ExperimentCase &&) { cancel = true; });
    check(stopped.cancelled && stopped.completed == 1, "Stop between cases");
    size_t calls = 0;
    stopped = run_experiment(p, e, &cancel, [&](ExperimentCase &&) { ++calls; });
    check(stopped.cancelled && !calls, "Stop before series");
    auto recovery = e;
    recovery.axes.front().values = {-1, 1000};
    size_t successful_after_failure = 0;
    report = run_experiment(p, recovery, nullptr, [&](ExperimentCase &&c) {
        if (c.index >= 2 && c.error_code.empty()) ++successful_after_failure;
    });
    check(report.failed == 2 && successful_after_failure == 2, "Fault prevented later valid cases");
    auto q = nested();
    const auto before = q;
    Document document(q);
    const auto circuit_before = document.root_project();
    document.navigate({id(50)});
    document.set_experiments({e});
    check(document.root_project().experiments.size() == 1 && document.location() == std::vector<std::string>{id(50)},
          "Experiment edit from nested view");
    check(same_simulation(circuit_before, document.root_project()), "Experiment edit invalidated simulation");
    document.undo();
    check(document.root_project().experiments.empty(), "Experiment Undo");
    document.redo();
    check(document.root_project().experiments.size() == 1, "Experiment Redo");
    ParameterTarget public_target{{}, id(50), "parameter/" + id(34)};
    auto changed = experiment_project(q, {{public_target, 330}});
    const auto first_id = expanded_uuid({id(50), id(41)}, id(31));
    const auto second_id = expanded_uuid({id(51), id(41)}, id(31));
    auto component = [&](const std::string &uuid) -> const Component & {
        return *std::find_if(changed.components.begin(), changed.components.end(),
                             [&](const auto &c) { return c.id == uuid; });
    };
    near(component(first_id).value, 330);
    near(component(second_id).value, 200);
    check(q == before, "Nested source mutated");
    const auto direct = resolve_parameter(q, {{id(50), id(41)}, id(31), "value"});
    check(direct == resolve_parameter(q, public_target), "Nested public binding");
    bool duplicate = false;
    try {
        experiment_project(q, {{public_target, 100}, {{{id(50), id(41)}, id(31), "value"}, 200}});
    } catch (const Diagnostic &d) {
        duplicate = d.code == "duplicate_sweep_target";
    }
    check(duplicate, "Conflicting aliases accepted");
    near(sweep_values(1, 1000, 4, true)[2], 100);
    near(sweep_values(10, 0, 3)[1], 5);
    Experiment many;
    many.id = id(100);
    many.name = "1000 no-recording cases";
    many.axes = {{{{}, id(5), "value"}, sweep_values(1000, 2000, 1000)}};
    p.profile.stop = 1e-5;
    report = run_experiment(p, many, nullptr, [&](ExperimentCase &&c) {
        check(!c.curves && c.measurements.empty() && c.error_code.empty(), "No-recording sweep");
    });
    check(report.completed == 1000 && !report.failed, "1000-case completion");
    auto invalid = many;
    invalid.axes.push_back({{{}, id(4), "value"}, sweep_values(1, 2, 101)});
    bool rejected = false;
    try {
        (void)experiment_size(invalid);
    } catch (const Diagnostic &) {
        rejected = true;
    }
    check(rejected, "Unbounded Cartesian product");
    std::cout
        << "PASS sweep ordering, scenarios, RC, failures, repeatability, hierarchy, persistence and Stop\n";
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
