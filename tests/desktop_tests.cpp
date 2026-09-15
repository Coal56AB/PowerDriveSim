#include "apps/desktop/editor.hpp"
#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsPathItem>
#include <QPainterPath>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QWheelEvent>
#include <QtTest/QtTest>
#include <cmath>
using namespace pds;
using namespace pds::desktop;
class DesktopTests : public QObject {
    Q_OBJECT
  private slots:
    void create_connect_edit_run_save_recover() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        EditorWindow window("ru", temp.path());
        window.show();
        QTest::qWait(50);
        auto *library = window.findChild<QTreeWidget *>("library");
        QVERIFY(library);
        QCOMPARE(library->topLevelItemCount(), 5);
        QVERIFY(window.scope() == nullptr);
        QVERIFY(!window.project().scope_enabled);
        auto place = [&](int kind, QPointF point) {
            QTreeWidgetItem *chosen = nullptr;
            for (int i = 0; i < library->topLevelItemCount(); ++i)
                for (int j = 0; j < library->topLevelItem(i)->childCount(); ++j) {
                    auto *item = library->topLevelItem(i)->child(j);
                    if (item->data(0, Qt::UserRole).toInt() == kind)
                        chosen = item;
                }
            if (!chosen)
                return;
            library->scrollToItem(chosen);
            QTest::mouseClick(library->viewport(),Qt::LeftButton,Qt::NoModifier,library->visualItemRect(chosen).center());
            QTest::mouseDClick(library->viewport(), Qt::LeftButton, Qt::NoModifier,
                              library->visualItemRect(chosen).center());
            QTest::mouseClick(window.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                              window.canvas()->mapFromScene(point));
        };
        place(3, {-40, 20});
        place(0, {200, 20});
        place(1, {200, 180});
        place(100, {-40, 280});
        QCOMPARE(window.project().components.size(), size_t(3));
        QCOMPARE(window.project().nodes.size(), size_t(1));
        auto v = window.project().components[0].id, r = window.project().components[1].id,
             c = window.project().components[2].id, g = window.project().nodes[0].id;
        auto wire = [&](Endpoint a, Endpoint b) {
            QTest::mousePress(window.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                              window.canvas()->mapFromScene(window.port_position(a)));
            QTest::mouseMove(window.canvas()->viewport(),window.canvas()->mapFromScene(window.port_position(b)));
            QTest::mouseRelease(window.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                              window.canvas()->mapFromScene(window.port_position(b)));
        };
        wire({v, "p"}, {r, "p"});
        wire({r, "n"}, {c, "p"});
        wire({v, "n"}, {g, "node"});
        wire({c, "n"}, {g, "node"});
        QCOMPARE(window.project().wires.size(), size_t(4));
        auto *value = window.findChild<QLineEdit *>("property_value");
        auto *apply = window.findChild<QPushButton *>("apply_properties");
        window.select_object(r);
        value->setText("2kOhm");
        QTest::mouseClick(apply, Qt::LeftButton);
        QCOMPARE(window.project().components[1].value, 2000.0);
        window.undo();
        QCOMPARE(window.project().components[1].value, 1000.0);
        window.redo();
        QCOMPARE(window.project().components[1].value, 2000.0);
        window.undo();
        value->setText("not a resistance");
        QTest::mouseClick(apply, Qt::LeftButton);
        QCOMPARE(window.project().components[1].value, 1000.0);
        QVERIFY(window.findChild<QListWidget *>("diagnostics_list")->count() > 0);
        window.observe_object(window.project().wires[1].id);
        window.observe_object(v);
        QVERIFY(window.scope() != nullptr);
        window.findChild<QLineEdit *>("sim_stop")->setText("5ms");
        window.findChild<QLineEdit *>("sim_step")->setText("1us");
        window.findChild<QAction *>("action_run")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(window.has_result(), 10000);
        QVERIFY(!window.result().cancelled);
        QCOMPARE(window.result().accepted_steps, size_t(5000));
        auto graph = resolve_connections(window.project());
        auto output = graph.nets.at(endpoint_key({c, "p"}));
        size_t channel = 0;
        while (channel < window.result().channels.size() &&
               window.result().channels[channel].object != output)
            ++channel;
        QVERIFY(channel < window.result().channels.size());
        QVERIFY(std::abs(window.result().samples.back().values[channel] - (1 - std::exp(-5.0))) < 2e-5);
        QTest::mouseClick(window.scope(), Qt::LeftButton, Qt::NoModifier, {200, 60});
        QTest::mouseClick(window.scope(), Qt::RightButton, Qt::NoModifier, {400, 60});
        QVERIFY(window.project().cursor_a >= 0);
        QVERIFY(window.project().cursor_b > window.project().cursor_a);
        const auto screenshot = qEnvironmentVariable("PDS_SCREENSHOT_PATH");
        if (!screenshot.isEmpty()) {
            window.select_object(c);
            QTest::qWait(50);
            QVERIFY(window.grab().save(screenshot));
        }
        auto expected = window.project();
        QVERIFY(window.save_project(temp.filePath("circuit.pds")));
        window.add_component(Kind::resistor, {420, 180});
        QVERIFY(window.autosave());
        QVERIFY(window.open_project(temp.filePath("circuit.pds")));
        QCOMPARE(window.project().components.size(), size_t(3));
        QCOMPARE(window.project().wires[0].id, expected.wires[0].id);
        QCOMPARE(window.project().cursor_a, expected.cursor_a);
        QVERIFY(window.recover(window.recovery_path()));
        QCOMPARE(window.project().components.size(), size_t(4));
        QVERIFY(window.windowTitle().endsWith("*"));
    }
    void stop_worker_and_diagnostic() {
        QTemporaryDir temp;
        EditorWindow window("en", temp.path());
        window.show();
        QVERIFY(window.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        window.findChild<QLineEdit *>("sim_stop")->setText("100s");
        window.findChild<QLineEdit *>("sim_step")->setText("1us");
        int ticks = 0;
        QTimer timer;
        connect(&timer, &QTimer::timeout, [&] {
            ++ticks;
            if (ticks == 3)
                window.stop_simulation();
        });
        timer.start(10);
        window.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(window.has_result(), 5000);
        QVERIFY(ticks >= 3);
        QVERIFY(window.result().cancelled);
        QVERIFY(window.result().accepted_steps < 100000000);
        QVERIFY(window.result().samples.empty());
        QVERIFY(window.result().channels.empty());
        QVERIFY(window.scope() == nullptr);
        timer.stop();
        auto malformed = window.project();
        malformed.wires.clear();
        window.set_project(malformed);
        window.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!window.running(), 5000);
        QTest::qWait(20);
        QVERIFY(!window.has_result());
        auto *errors = window.findChild<QListWidget *>("diagnostics_list");
        QVERIFY(errors->count() > 0);
        auto *item = errors->item(0);
        QTest::mouseClick(errors->viewport(), Qt::LeftButton, Qt::NoModifier,
                          errors->visualItemRect(item).center());
        QVERIFY(!window.canvas()->scene()->selectedItems().empty());
    }
    void gates_probes_and_scope() {
        QTemporaryDir temp;
        EditorWindow window("en", temp.path());
        window.show();
        auto v = window.add_component(Kind::voltage, {0, 0});
        auto sw = window.add_component(Kind::ideal_switch, {220, 0});
        auto ip = window.add_component(Kind::current_probe, {440, 0});
        auto r = window.add_component(Kind::resistor, {440, 160});
        auto vp = window.add_component(Kind::voltage_probe, {220, 160});
        auto ground = window.add_node(true, {0, 240});
        auto pattern = window.add_pattern({0, -140});
        QVERIFY(window.connect_ports({v, "p"}, {sw, "p"}));
        QVERIFY(window.connect_ports({sw, "n"}, {ip, "p"}));
        QVERIFY(window.connect_ports({ip, "n"}, {r, "p"}));
        QVERIFY(window.connect_ports({r, "n"}, {ground, "node"}));
        QVERIFY(window.connect_ports({v, "n"}, {ground, "node"}));
        QVERIFY(window.connect_ports({vp, "p"}, {r, "p"}));
        QVERIFY(window.connect_ports({vp, "n"}, {ground, "node"}));
        size_t count = window.project().wires.size();
        QVERIFY(!window.connect_ports({vp, "out"}, {sw, "gate"}));
        QCOMPARE(window.project().wires.size(), count);
        QVERIFY(window.connect_ports({pattern, "out"}, {sw, "gate"}));
        window.select_object(pattern);
        window.findChild<QCheckBox *>("property_closed")->setChecked(true);
        window.findChild<QPlainTextEdit *>("property_events")->setPlainText("1ms 0\n2ms 1");
        QTest::mouseClick(window.findChild<QPushButton *>("apply_properties"), Qt::LeftButton);
        QCOMPARE(window.project().events.size(), size_t(2));
        window.set_scope_enabled(true);
        auto *selected_channels = window.findChild<QListWidget *>("channels");
        for (int i = 0; i < selected_channels->count(); ++i) {
            auto *item = selected_channels->item(i);
            auto key = item->data(Qt::UserRole).toString().toStdString();
            item->setCheckState(key == vp || key == ip || key == "gate/" + sw ? Qt::Checked : Qt::Unchecked);
        }
        window.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(window.has_result(), 10000);
        QCOMPARE(window.result().gate_objects.size(), size_t(1));
        bool off = false, on_again = false;
        for (const auto &sample : window.result().samples) {
            if (sample.time >= .001 && sample.time < .002) {
                QVERIFY(!sample.gates[0]);
                off = true;
            }
            if (sample.time >= .002) {
                QVERIFY(sample.gates[0]);
                on_again = true;
            }
        }
        QVERIFY(off);
        QVERIFY(on_again);
        size_t vi = 0, ii = 0;
        while (vi < window.result().channels.size() && window.result().channels[vi].object != vp)
            ++vi;
        while (ii < window.result().channels.size() && window.result().channels[ii].object != ip)
            ++ii;
        QVERIFY(vi < window.result().channels.size());
        QVERIFY(ii < window.result().channels.size());
        QVERIFY(std::abs(window.result().samples.back().values[vi] - 1) < 1e-9);
        QVERIFY(std::abs(window.result().samples.back().values[ii] - .001) < 1e-12);
        auto *channels = window.findChild<QListWidget *>("channels");
        for (int i = 0; i < channels->count(); ++i) {
            auto *item = channels->item(i);
            auto key = item->data(Qt::UserRole).toString().toStdString();
            item->setCheckState(key == vp || key == ip || key == "gate/" + sw ? Qt::Checked : Qt::Unchecked);
        }
        QCOMPARE(window.project().scope_channels.size(), size_t(3));
        double before = window.scope()->end - window.scope()->begin;
        QPointF at(300, 80);
        QWheelEvent wheel(at, window.scope()->mapToGlobal(at.toPoint()), QPoint(), QPoint(0, 120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(window.scope(), &wheel);
        QVERIFY(window.project().scope_end - window.project().scope_begin < before);
        window.scope()->fit();
        QVERIFY(std::abs(window.project().scope_end - .01) < 1e-12);
        auto saved = window.project();
        QVERIFY(window.save_project(temp.filePath("gate.pds")));
        QVERIFY(window.open_project(temp.filePath("gate.pds")));
        QCOMPARE(window.project().patterns[0].id, pattern);
        QCOMPARE(window.project().scope_channels, saved.scope_channels);
        QCOMPARE(window.project().events.size(), size_t(2));
    }
    void independent_plots_and_recording_off() {
        QTemporaryDir temp;
        EditorWindow window("ru", temp.path());
        window.resize(1520, 940);
        window.show();
        QVERIFY(window.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        auto probe = window.add_component(Kind::voltage_probe, {540, 80});
        auto output = window.project().nodes[2].id, ground = window.project().nodes[0].id;
        QVERIFY(window.connect_ports({probe, "p"}, {output, "node"}));
        QVERIFY(window.connect_ports({probe, "n"}, {ground, "node"}));
        auto first = window.add_plot({780, 80});
        auto second = window.add_plot({780, 280});
        QVERIFY(window.connect_ports({probe, "out"}, {first, "in1"}));
        QVERIFY(window.connect_ports({probe, "out"}, {second, "in2"}));
        window.findChild<QAction *>("action_fit")->trigger();
        QVERIFY(window.scope() == nullptr);
        window.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(window.has_result(), 10000);
        QCOMPARE(window.result().channels.size(), size_t(1));
        QCOMPARE(window.result().channels[0].object, probe);
        QVERIFY(window.result().gate_objects.empty());
        QVERIFY(!window.result().samples.empty());
        window.open_plot(first);
        window.open_plot(second);
        auto *a = window.findChild<QDialog *>("plot_" + QString::fromStdString(first));
        auto *b = window.findChild<QDialog *>("plot_" + QString::fromStdString(second));
        QVERIFY(a && b && a != b);
        auto *av = a->findChild<Scope *>("scope");
        auto *bv = b->findChild<Scope *>("scope");
        QVERIFY(av && bv && av != bv);
        QTest::mouseClick(av, Qt::LeftButton, Qt::NoModifier, {260, 100});
        QVERIFY(window.project().plots[0].cursor_a >= 0);
        QCOMPARE(window.project().plots[1].cursor_a, -1.0);
        const auto screenshot = qEnvironmentVariable("PDS_PLOT_SCREENSHOT_PATH");
        if (!screenshot.isEmpty()) {
            QTest::qWait(40);
            QVERIFY(a->grab().save(screenshot));
        }
        a->close();
        b->close();
        window.observe_object(window.project().wires.front().id);
        window.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(window.has_result(), 10000);
        QVERIFY(window.result().channels.size() > 1);
        window.set_scope_enabled(false);
        QVERIFY(window.scope() == nullptr);
        QCOMPARE(window.result().channels.size(), size_t(1));
        window.select_object(first);
        window.findChild<QAction *>("action_delete")->trigger();
        window.select_object(second);
        window.findChild<QAction *>("action_delete")->trigger();
        window.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(window.has_result(), 10000);
        QVERIFY(window.result().samples.empty());
        QVERIFY(window.result().channels.empty());
    }
    void wire_tool_and_empty_state() {
        QTemporaryDir temp;
        EditorWindow window("ru", temp.path());
        window.show();
        QTest::qWait(30);
        QVERIFY(window.scope() == nullptr);
        QVERIFY(!window.findChild<QAction *>("action_export"));
        auto *library = window.findChild<QTreeWidget *>("library");
        for (int i = 0; i < library->topLevelItemCount(); ++i)
            for (int j = 0; j < library->topLevelItem(i)->childCount(); ++j)
                QVERIFY(library->topLevelItem(i)->child(j)->data(0, Qt::UserRole).toInt() != 101);
        window.findChild<QAction *>("action_connect_tool")->trigger();
        QTest::mouseClick(window.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                          window.canvas()->mapFromScene(QPointF(0, 100)));
        QCOMPARE(window.project().nodes.size(), size_t(1));
        auto resistor = window.add_component(Kind::resistor, {200, 100});
        QTest::mousePress(window.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                          window.canvas()->mapFromScene(window.port_position({resistor, "p"})));
        QTest::mouseMove(window.canvas()->viewport(),window.canvas()->mapFromScene(QPointF(100,220)));
        QTest::mouseRelease(window.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                          window.canvas()->mapFromScene(QPointF(100, 220)));
        QCOMPARE(window.project().nodes.size(), size_t(2));
        QCOMPARE(window.project().wires.size(), size_t(1));
        window.stop_simulation();
    }
    void gestures_taps_and_transforms(){
        QTemporaryDir temp;EditorWindow window("ru",temp.path());window.show();QTest::qWait(30);
        auto r=window.add_component(Kind::resistor,{100,80});auto c=window.add_component(Kind::capacitor,{380,80});auto plot=window.add_plot({320,240});
        QVERIFY(window.connect_ports({r,"n"},{c,"p"}));
        auto wire=window.project().wires.front().id;QPointF middle;
        for(auto* item:window.canvas()->scene()->items())if(item->data(0).toString().toStdString()==wire){auto* path=dynamic_cast<QGraphicsPathItem*>(item);if(path)middle=path->path().pointAtPercent(.5);}
        auto* viewport=window.canvas()->viewport();auto from=window.canvas()->mapFromScene(window.port_position({plot,"in1"}));auto end=window.canvas()->mapFromScene(middle);
        QTest::mousePress(viewport,Qt::LeftButton,Qt::NoModifier,from);QTest::mouseMove(viewport,end);QTest::mouseRelease(viewport,Qt::LeftButton,Qt::NoModifier,end);
        QCOMPARE(window.project().wires.size(),size_t(3));QCOMPARE(window.project().nodes.size(),size_t(1));QCOMPARE(plot_channels(window.project(),plot).size(),size_t(1));
        window.undo();QCOMPARE(window.project().wires.size(),size_t(1));window.redo();
        window.select_object(r);auto before=window.port_position({r,"p"});window.findChild<QAction*>("action_rotate")->trigger();auto after=window.port_position({r,"p"});QVERIFY(before!=after);QCOMPARE(after,QPointF(100,20));
        window.findChild<QAction*>("action_mirror")->trigger();QVERIFY(window.project().components.front().orientation.mirrored);
        window.findChild<QAction*>("action_copy")->trigger();window.findChild<QAction*>("action_paste")->trigger();QCOMPARE(window.project().components.size(),size_t(3));
        QVERIFY(window.save_project(temp.filePath("edited.pds")));QVERIFY(window.open_project(temp.filePath("edited.pds")));QVERIFY(window.project().components.front().orientation.mirrored);
    }
    void delete_disconnect_undo() {
        QTemporaryDir temp;
        EditorWindow window("en", temp.path());
        window.show();
        QVERIFY(window.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        auto wire = window.project().wires.front();
        size_t count = window.project().wires.size();
        window.select_object(wire.id);
        window.findChild<QAction *>("action_delete")->trigger();
        QCOMPARE(window.project().wires.size(), count - 1);
        window.undo();
        QCOMPARE(window.project().wires.size(), count);
        QCOMPARE(window.project().wires.front().id, wire.id);
        window.redo();
        QCOMPARE(window.project().wires.size(), count - 1);
    }
};
QTEST_MAIN(DesktopTests)
#include "desktop_tests.moc"
