#include "core/editor/document.hpp"
#include "formats/project/project.hpp"
#include "results/measurements.hpp"
#include <cmath>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <thread>
using namespace pds;
static void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
static void near(double a, double b) {
    check(std::abs(a - b) < 1e-10, "Measurement mismatch");
}
int main(int argc, char **argv) {
    try {
        if (argc < 2)
            return 2;
        Result r;
        r.channels = {{"a", "A", "V"}, {"b", "B", "V"}, {"c", "C", "A"}};
        for (int i = 0; i < 5; ++i)
            r.samples.push_back({double(i), {double(i), double(i * 2), double(i)}, {}});
        auto a = cursor_value(r, 0, 1.4), b = cursor_value(r, 1, 3.2);
        check(a && b, "Cursor lookup");
        near(a->time, 1);
        near(b->value, 6);
        auto math = compare_cursors(*a, *b);
        near(math.dt, 2);
        near(*math.dy, 5);
        near(*math.frequency, .5);
        near(*math.slope, 2.5);
        check(!compare_cursors(*a, *cursor_value(r, 2, 3)).dy, "Different units are not subtracted");
        check(!compare_cursors(*a, *a).frequency, "Zero interval has no frequency");
        check(!cursor_value(r, 0, -1), "Unset cursor");
        auto s = signal_statistics(r, 0, 0, 4);
        near(s.mean, 2);
        near(s.median, 2);
        near(s.rms, std::sqrt(6));
        near(s.standard_deviation, std::sqrt(2));
        near(s.minimum_time, 0);
        near(s.maximum_time, 4);
        s = signal_statistics(r, 0, 1, 2);
        near(s.median, 1.5);
        check(s.count == 2, "Inclusive measurement range");
        Result peaks;
        peaks.channels = {{"a", "A", "V"}};
        const double values[] = {0, 2, 2, 0, 4, 0, 3, 0};
        for (int i = 0; i < 8; ++i)
            peaks.samples.push_back({double(i), {values[i]}, {}});
        auto found = signal_peaks(peaks, 0, 0, 7, 1, 1, 0, 10);
        check(found.size() == 3, "Plateau peak counted once");
        near(found[0].time, 4);
        near(found[2].time, 1.5);
        found = signal_peaks(peaks, 0, 0, 7, 1, 1, 3, 10);
        check(found.size() == 1, "Peak distance filtering keeps highest peak");
        Result pulse;
        pulse.gate_objects = {"gate"};
        for (int i = 0; i <= 100; ++i)
            pulse.samples.push_back({i * .1, {}, {i % 20 >= 5 && i % 20 < 10}});
        auto m = pulse_measurements(pulse, 0, 0, 10);
        check(m.cycles == 4, "Complete digital cycles");
        near(*m.period, 2);
        near(*m.frequency, .5);
        near(*m.duty, 25);
        near(*m.high_width, .5);
        near(*m.rise_time, 0);
        near(*m.fall_time, 0);
        Result ramp;
        ramp.channels = {{"a", "A", "V"}};
        ramp.samples = {{0, {0}, {}}, {1, {1}, {}}, {2, {1}, {}}, {3, {0}, {}}, {4, {0}, {}}, {5, {1}, {}}};
        auto analog = pulse_measurements(ramp, 0, 0, 5, std::pair{0., 1.});
        near(*analog.rise_time, .8);
        near(*analog.fall_time, .8);
        near(*analog.period, 4);
        Result fall_first;
        fall_first.gate_objects = {"g"};
        fall_first.samples = {{0, {}, {true}}, {1, {}, {false}}, {2, {}, {true}}, {3, {}, {false}}};
        auto fall_cycle = pulse_measurements(fall_first, 0, 0, 3);
        near(*fall_cycle.period, 2);
        near(*fall_cycle.duty, 50);
        std::ifstream file(std::string(argv[1]) + "/examples/diode-freewheel.pds");
        auto p = read_project(file);
        p.profile.stop = .03;
        p.profile.step = 1e-5;
        auto ir = compile(p);
        auto expected = execute(ir);
        std::vector<Sample> received;
        size_t chunks = 0;
        auto tail = execute(ir, nullptr, nullptr, nullptr, nullptr, [&](Result &&batch) {
            ++chunks;
            received.insert(received.end(), std::make_move_iterator(batch.samples.begin()),
                            std::make_move_iterator(batch.samples.end()));
        });
        received.insert(received.end(), std::make_move_iterator(tail.samples.begin()),
                        std::make_move_iterator(tail.samples.end()));
        check(chunks > 0 && received.size() == expected.samples.size(),
              "Streaming has no missing or duplicated samples");
        for (size_t i = 0; i < received.size(); ++i)
            check(received[i].time == expected.samples[i].time &&
                      received[i].values == expected.samples[i].values &&
                      received[i].gates == expected.samples[i].gates,
                  "Streaming is bitwise identical");
        check(tail.accepted_steps == expected.accepted_steps && tail.linear_solves == expected.linear_solves,
              "Streaming preserves solver metrics");
        std::atomic_bool paused{true}, cancel{false};
        std::atomic<double> time{-1};
        auto worker =
            std::async(std::launch::async, [&] { return execute(ir, &cancel, &time, nullptr, &paused); });
        check(worker.wait_for(std::chrono::milliseconds(40)) == std::future_status::timeout,
              "Paused solver must wait");
        near(time.load(), 0);
        paused = false;
        check(worker.wait_for(std::chrono::seconds(3)) == std::future_status::ready, "Resume completes");
        auto resumed = worker.get();
        check(resumed.samples.back().values == expected.samples.back().values,
              "Pause does not change result");
        paused = true;
        cancel = false;
        worker =
            std::async(std::launch::async, [&] { return execute(ir, &cancel, &time, nullptr, &paused); });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        cancel = true;
        check(worker.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready,
              "Stop interrupts pause");
        check(worker.get().cancelled, "Stopped paused result marked cancelled");
        Document doc(p);
        auto original_document = doc.project();
        auto id = p.components.front().id;
        doc.apply("label", [&](Project &q) { q.labels.push_back({id, "name", 20, -40, {1, true}}); });
        ViewOptions options;
        options.cursor_channel_a = "channel/a";
        options.cursor_channel_b = "gate/b";
        options.free_cursors = true;
        options.cursor_y_a = 4.5;
        options.y_high = 10;
        options.time_span = .01;
        options.display_columns = 2;
        options.signal_displays = {{"channel/a", 0}, {"gate/b", 1}};
        options.hidden_channels = {"gate/b"};
        options.curve_names = {{"channel/a", "Load voltage"}};
        options.curve_styles = {{"channel/a", CurveLine::dash, 2.5, CurveMarker::triangle, 8}};
        options.legend_positions = {{0, .25, .75}, {1, 1, 0}};
        doc.set_view_options(options);
        std::ostringstream out;
        write_project(doc.project(), out);
        std::istringstream in(out.str());
        auto restored = read_project(in);
        check(restored.labels == doc.project().labels, "Label layout round trip");
        check(restored.view_options == doc.project().view_options, "Scope settings round trip");
        check(same_simulation(original_document, restored),
              "Labels and views do not change simulation identity");
        doc.undo();
        check(doc.project().labels.empty(), "Label undo");
        check(doc.project().view_options[0] == options, "Undo preserves view settings");
        doc.redo();
        auto fragment = doc.copy({id});
        check(fragment.labels.size() == 1, "Copy includes label layout");
        auto pasted = doc.paste(fragment, 400, 400);
        check(doc.project().labels.size() == 2 && doc.project().labels.back().object != id,
              "Paste remaps label owner");
        std::cout << "PASS measurements, cursor units, digital and analog edges, streaming determinism, "
                     "pause/stop, label persistence\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
