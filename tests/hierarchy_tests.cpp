#include "core/editor/document.hpp"
#include "core/model/hierarchy.hpp"
#include "core/solver/reference/reference.hpp"
#include "formats/project/project.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
using namespace pds;
static std::string id(int n) {
    return derived_uuid("hierarchy-test:" + std::to_string(n));
}
static void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
static void error(const std::string &code, const std::function<void()> &operation) {
    try {
        operation();
    } catch (const Diagnostic &e) {
        if (e.code == code)
            return;
        throw;
    }
    throw std::runtime_error("Missing diagnostic: " + code);
}
static void wire(Schematic &p, Endpoint a, Endpoint b) {
    p.wires.push_back({new_uuid(), a, b, {}});
}
static Project fixture() {
    Project p;
    p.id = id(0);
    p.name = "Two independent RC circuits";
    p.wired = true;
    p.profile = {.001, 1e-6, Method::trapezoidal};
    p.nodes = {{id(1), "Ground", true}, {id(2), "Supply", false}};
    p.components = {{id(3), "V", Kind::voltage, "", "", 10}};
    wire(p, {id(3), "p"}, {id(2), "node"});
    wire(p, {id(3), "n"}, {id(1), "node"});
    Definition d;
    d.id = id(10);
    d.name = "RC";
    d.wired = true;
    d.nodes = {{id(11), "Out", false, 100, 0}};
    d.components = {{id(12), "R", Kind::resistor, "", "", 1000, 0, -80, 0},
                    {id(13), "C", Kind::capacitor, "", "", 1e-6, 2, 100, 100}};
    wire(d, {id(12), "n"}, {id(11), "node"});
    wire(d, {id(13), "p"}, {id(11), "node"});
    d.ports = {
        {id(14), "in", {id(12), "p"}}, {id(15), "ground", {id(13), "n"}}, {id(16), "out", {id(11), "node"}}};
    d.parameters = {{id(17), "Resistance", "Ohm", id(12), "value", 1000}};
    d.extensions = {"x-library \"test\""};
    p.definitions.push_back(d);
    p.instances = {{id(20), "First", d.id, -200, 0}, {id(21), "Second", d.id, 200, 0}};
    p.instances[1].parameters = {{id(17), 2000}};
    for (const auto &i : p.instances) {
        wire(p, {i.id, id(14)}, {id(2), "node"});
        wire(p, {i.id, id(15)}, {id(1), "node"});
    }
    return p;
}
static double output(const Result &result, const std::string &object) {
    auto found = std::find_if(result.channels.begin(), result.channels.end(),
                              [&](const auto &c) { return c.object == object; });
    require(found != result.channels.end(), "Missing recorded object");
    return result.samples.back().values.at(size_t(found - result.channels.begin()));
}
static Project nested() {
    auto p = fixture();
    Definition wrapper;
    wrapper.id = id(30);
    wrapper.name = "Nested RC";
    wrapper.wired = true;
    wrapper.instances = {{id(31), "Inner", id(10), 40, 20}};
    wrapper.ports = {{id(14), "in", {id(31), id(14)}},
                     {id(15), "ground", {id(31), id(15)}},
                     {id(16), "out", {id(31), id(16)}}};
    wrapper.parameters = {{id(17), "Resistance", "Ohm", id(31), id(17), 1000}};
    p.definitions.push_back(wrapper);
    for (auto &i : p.instances)
        i.definition = wrapper.id;
    return p;
}
int main() try {
    auto p = fixture();
    auto flat = flatten(p);
    require(flat.project.components.size() == 5 && flat.project.instances.empty() &&
                flat.project.definitions.empty(),
            "Atomic expansion");
    auto ir = compile(p);
    auto result = execute(ir);
    for (int n = 0; n < 2; ++n) {
        const auto node = expanded_uuid({id(20 + n)}, id(11));
        const auto expected = 10 - 8 * std::exp(-p.profile.stop / (.001 * (n + 1)));
        require(std::abs(output(result, node) - expected) < 1e-5,
                "Independent analytical RC states and parameters");
        require(ir.origins.at(node) == ObjectPath{{id(20 + n)}, id(11)}, "Source mapping");
    }
    auto permuted = p;
    auto oriented = p;
    oriented.instances[0].orientation = {1, true};
    oriented.definitions[0].components[1].orientation = {1, false};
    auto geometry = flatten(oriented).project;
    auto rotated = std::find_if(geometry.components.begin(), geometry.components.end(),
                                [&](const auto &c) { return c.id == expanded_uuid({id(20)}, id(13)); });
    require(rotated != geometry.components.end() && rotated->x == -100 && rotated->y == 100 &&
                rotated->orientation == Orientation{2, true},
            "Flatten uses the canvas rotation/mirror convention");
    std::reverse(permuted.instances.begin(), permuted.instances.end());
    std::reverse(permuted.wires.begin(), permuted.wires.end());
    require(execute(compile(permuted)).samples.back().values == result.samples.back().values,
            "Order independent expansion");
    p = nested();
    auto expanded = flatten(p);
    const auto internal = expanded_uuid({id(20), id(31)}, id(13));
    require(expanded.origins.at(internal).instances.size() == 2, "Nested origins");
    p.instances[0].name = "Renamed";
    p.instances[0].x += 123;
    p.definitions[0].name = "Other library name";
    require(flatten(p).origins.count(internal) == 1, "UUID stability after names and geometry edits");
    p.instances[0].orientation = {1, true};
    p.labels.push_back({id(20), "name", 20, 10, {2, false}});
    std::ostringstream out;
    write_project(p, out);
    std::istringstream in(out.str());
    auto loaded = read_project(in);
    require(loaded == p, "Hierarchy, parameters, geometry, extensions round trip");
    require(execute(compile(loaded)).samples.back().values == execute(compile(p)).samples.back().values,
            "Round trip execution");
    auto bad = p;
    bad.definitions[0].instances.push_back({id(40), "Cycle", id(30)});
    error("recursive_hierarchy", [&] { flatten(bad); });
    bad = p;
    bad.instances[0].definition = id(99);
    error("missing_definition", [&] { compile(bad); });
    bad = p;
    bad.definitions[0].ports[0].domain = Domain::gate;
    error("incompatible_public_port", [&] { flatten(bad); });
    bad = p;
    bad.definitions[0].ports[0].terminal.port = "unknown";
    error("missing_port", [&] { flatten(bad); });
    bad = p;
    bad.instances[0].parameters = {{id(99), 3}};
    error("invalid_instance_parameter", [&] { compile(bad); });
    bad = p;
    bad.instances[0].parameters = {{id(17), -1}};
    try {
        compile(bad);
        throw std::runtime_error("Missing invalid parameter diagnostic");
    } catch (const Diagnostic &e) {
        require(e.code == "invalid_parameter" && e.object == id(12) &&
                    e.path == std::vector<std::string>{id(20), id(31)},
                "Addressed nested diagnostic");
    }
    Document doc(p);
    auto baseline = execute(compile(doc.project()));
    doc.detach_instance(id(20));
    auto detached = doc.project();
    require(detached.instances[0].definition != detached.instances[1].definition, "Detached root definition");
    auto child = definition(detached, detached.instances[0].definition).instances[0].definition;
    require(child != id(10), "Detached nested definition");
    require(execute(compile(detached)).samples.back().values == baseline.samples.back().values,
            "Detach preserves UUIDs and physics");
    doc.edit_definition(child, [](Definition &d) { d.components[1].value *= 2; });
    auto changed = execute(compile(doc.project()));
    require(output(changed, expanded_uuid({id(20), id(31)}, id(11))) !=
                output(baseline, expanded_uuid({id(20), id(31)}, id(11))),
            "Detached circuit edited");
    require(output(changed, expanded_uuid({id(21), id(31)}, id(11))) ==
                output(baseline, expanded_uuid({id(21), id(31)}, id(11))),
            "Sibling remains independent");
    doc.undo();
    require(doc.project() == detached, "Definition undo");
    doc.undo();
    require(doc.project() == p, "Detach undo");
    doc.redo();
    doc.redo();
    auto before_expand = doc.project();
    doc.expand_instance(id(20));
    require(doc.project().instances.size() == 1 && doc.project().components.size() == 3,
            "Expand to editable atoms");
    require(execute(compile(doc.project())).samples.back().values == changed.samples.back().values,
            "Expansion preserves computation");
    doc.undo();
    require(doc.project() == before_expand, "Expansion undo");
    doc.redo();
    auto selection = std::vector<std::string>{expanded_uuid({id(20), id(31)}, id(12)),
                                              expanded_uuid({id(20), id(31)}, id(13)),
                                              expanded_uuid({id(20), id(31)}, id(11))};
    auto grouped = doc.create_definition(selection, "Selected RC");
    require(doc.project().instances.size() == 2, "Group selected atoms");
    auto grouped_result = execute(compile(doc.project()));
    require(std::abs(output(grouped_result, expanded_uuid({grouped}, selection[2])) -
                     output(changed, selection[2])) < 1e-10,
            "Grouping preserves physical response");
    auto fragment = Document(nested()).copy({id(21)});
    auto altered = nested();
    altered.definitions[0].components[1].value *= 3;
    Document destination(altered);
    auto inserted = destination.paste(fragment, 0, 400);
    require(inserted.size() == 1, "Paste nested instance");
    auto imported = definition(destination.project(), destination.project().instances.back().definition)
                        .instances[0]
                        .definition;
    require(imported != id(10) && definition(destination.project(), imported).components[1].value == 1e-6,
            "Clipboard catalog conflicts preserve imported nested definition");
    auto gated = fixture();
    auto &cell = gated.definitions[0];
    cell.components.push_back({id(50), "Switch", Kind::ideal_switch, "", "", 0});
    wire(cell, {id(50), "p"}, {id(13), "p"});
    wire(cell, {id(50), "n"}, {id(13), "n"});
    cell.ports.push_back({id(51), "gate", {id(50), "gate"}, Domain::gate, Direction::input});
    gated.patterns.push_back({id(52), "Gate", 0, 0, false});
    gated.events = {{.0005, id(52), true}};
    for (const auto &i : gated.instances)
        wire(gated, {id(52), "out"}, {i.id, id(51)});
    auto gates = compile(gated);
    require(std::count_if(gates.events.begin(), gates.events.end(),
                          [](const auto &e) { return e.time == .0005 && e.closed; }) == 3,
            "Public gates preserve simultaneous fanout and pattern observation");
    gated.plots.push_back({id(53), "Output", 0, 0, 1});
    wire(gated, {id(20), id(16)}, {id(53), "in1"});
    require(plot_channels(gated, id(53)) == std::vector<std::string>{expanded_uuid({id(20)}, id(11))},
            "External graph observes a public electrical port");
    auto text = out.str();
    text.erase(text.rfind("end_definition"));
    std::istringstream broken(text);
    error("parse_error", [&] { read_project(broken); });
    Document navigating(nested());
    const auto unedited = navigating.root_project();
    navigating.navigate({id(20), id(31)});
    require(navigating.project().id != navigating.root_project().id &&
                navigating.project().components.size() == 2,
            "Active hierarchy view");
    navigating.apply("Edit inside", [](Project &v) { v.components[0].value = 3000; });
    require(definition(navigating.root_project(), id(10)).parameters[0].value == 3000,
            "Internal bound edits update shared defaults");
    require(navigating.root_project().instances.size() == 2, "Editing view retains root instances");
    std::ostringstream whole;
    write_project(navigating.root_project(), whole);
    std::istringstream saved(whole.str());
    require(read_project(saved) == navigating.root_project(),
            "Save from an internal view retains the full project");
    navigating.undo();
    require(navigating.location().size() == 2 && navigating.root_project() == unedited,
            "Undo inside definition");
    navigating.undo();
    require(navigating.location().empty(), "Undo navigation");
    navigating.redo();
    require(navigating.location().size() == 2, "Redo navigation");
    navigating.redo();
    require(navigating.project().components[0].value == 3000, "Redo shared edit");
    navigating.apply("Record both instances", [](Project &p) {
        p.scope_enabled = true;
        p.scope_channels = {expanded_uuid({id(20), id(31)}, id(12)), expanded_uuid({id(21), id(31)}, id(12))};
    });
    const auto grouped_inside = navigating.create_definition({id(12)}, "Inner resistor");
    require(navigating.root_project().scope_channels ==
                std::vector<std::string>{expanded_uuid({id(20), id(31), grouped_inside}, id(12)),
                                         expanded_uuid({id(21), id(31), grouped_inside}, id(12))},
            "Grouping remaps recording in every linked instance");
    require(definition(navigating.root_project(), id(10)).ports[0].terminal.object == grouped_inside,
            "Grouping preserves enclosing public port");
    require(definition(navigating.root_project(), id(10)).parameters[0].object == grouped_inside,
            "Grouping preserves enclosing parameter binding");
    auto nested_before = execute(compile(navigating.root_project()));
    navigating.expand_instance(grouped_inside);
    auto nested_after = execute(compile(navigating.root_project()));
    require(nested_after.samples.back().values == nested_before.samples.back().values,
            "Nested expansion retains the full numerical result and UUID ordering");
    for (size_t c = 0; c < nested_before.channels.size(); ++c) {
        // Component UUIDs at the edited level change after explicit replacement;
        // unaffected output nodes retain their identities and physical response.
        auto object = nested_before.channels[c].object;
        if (object == expanded_uuid({id(20), id(31)}, id(11)) ||
            object == expanded_uuid({id(21), id(31)}, id(11)))
            require(std::abs(output(nested_before, object) - output(nested_after, object)) < 1e-10,
                    "Internal expansion preserves public bindings and physics");
    }
    std::cout << "Hierarchy: numerical, isolation, nesting, round trip, diagnostics, detach, grouping, "
                 "expansion, clipboard and history passed\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
