#include "apps/desktop/editor.hpp"
#include "apps/desktop/routing.hpp"
#include "apps/desktop/theme.hpp"
#include "core/model/hierarchy.hpp"
#include "formats/project/project.hpp"
#include "benchmarks/metrics.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStyleOptionGraphicsItem>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <sstream>
using namespace pds;
using namespace pds::desktop;
class InteractionTests : public QObject {
    Q_OBJECT
    static QGraphicsItem *item(EditorWindow &w, const std::string &id) {
        for (auto *o : w.canvas()->scene()->items())
            if (!o->parentItem() && o->data(1).toString() != "label" &&
                o->data(0).toString().toStdString() == id)
                return o;
        return nullptr;
    }
    static void ready(EditorWindow &w) {
        w.resize(1500, 1000);
        w.show();
        QTest::qWait(30);
        w.canvas()->resetTransform();
        w.canvas()->centerOn(180, 100);
        w.canvas()->setFocus();
    }
    static void drag(EditorWindow &w, QPointF a, QPointF b,
                     Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        auto *c = w.canvas();
        QTest::mousePress(c->viewport(), Qt::LeftButton, modifiers, c->mapFromScene(a));
        QTest::mouseMove(c->viewport(), c->mapFromScene(b), 5);
        QTest::mouseRelease(c->viewport(), Qt::LeftButton, modifiers, c->mapFromScene(b));
    }
    static std::string encoded(const Project &p) {
        std::ostringstream out;
        write_project(p, out);
        return out.str();
    }
  private slots:
    void thyristor_properties_and_run() {
        QTemporaryDir dir;
        EditorWindow w("ru", dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR "/examples/thyristor-halfwave.pds")));
        ready(w);
        auto *insert = w.findChild<QAction *>("insert_component_9");
        QVERIFY(insert && !insert->icon().isNull());
        const auto count = w.project().components.size();
        insert->trigger();
        QTest::mouseClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                          w.canvas()->mapFromScene(QPointF(300, 300)));
        QCOMPARE(w.project().components.size(), count + 1);
        QCOMPARE(w.project().components.back().kind, Kind::thyristor);
        w.undo(); QCOMPARE(w.project().components.size(), count);
        const auto id = std::find_if(w.project().components.begin(), w.project().components.end(),
                                     [](const auto &c) { return c.kind == Kind::thyristor; })->id;
        w.select_object(id);
        auto *holding = w.findChild<QLineEdit *>("property_holding_current");
        auto *latched = w.findChild<QCheckBox *>("property_initial_latched");
        auto *gate = w.findChild<QCheckBox *>("property_closed");
        QVERIFY(holding && holding->isVisible() && latched && latched->isVisible());
        QVERIFY(!gate->isVisible()); // The connected PWM owns the gate.
        const auto before = encoded(w.root_project());
        holding->setText("-1"); QTest::keyClick(holding, Qt::Key_Return);
        QCOMPARE(encoded(w.root_project()), before);
        holding->setText("30 mA"); QTest::keyClick(holding, Qt::Key_Return);
        auto device = [&]() -> const Component & {
            return *std::find_if(w.project().components.begin(), w.project().components.end(),
                                 [&](const auto &c) { return c.id == id; });
        };
        QCOMPARE(device().semiconductor.holding_current, .03);
        QTest::mouseClick(latched, Qt::LeftButton, Qt::NoModifier, QPoint(8, latched->height() / 2));
        QVERIFY(device().semiconductor.initial_latched);
        w.undo(); QVERIFY(!device().semiconductor.initial_latched);
        auto *mode = w.findChild<QComboBox *>("property_semiconductor_model");
        mode->setCurrentIndex(1); QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, 1));
        QVERIFY(w.findChild<QLineEdit *>("property_forward_voltage")->isVisible());
        auto *charge = w.findChild<QComboBox *>("property_charge_model");
        QVERIFY(!charge || !charge->isVisible());
        const auto path = dir.filePath("thyristor.pds");
        QVERIFY(w.save_project(path)); const auto expected = encoded(w.root_project());
        QVERIFY(w.open_project(path)); QCOMPARE(encoded(w.root_project()), expected);
        w.select_object(id);
        w.canvas()->fitInView(w.canvas()->scene()->itemsBoundingRect().adjusted(-60, -60, 60, 60), Qt::KeepAspectRatio);
        w.start_simulation(); QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 2000);
        QVERIFY(w.has_result() && !w.result().samples.empty());
        const auto plot = w.project().plots.front().id; w.open_plot(plot);
        auto *graph = w.findChild<QDialog *>("plot_" + QString::fromStdString(plot));
        QVERIFY(graph && graph->isVisible());
        auto *scope = graph->findChild<Scope *>("scope");
        QVERIFY(scope);
        QCOMPARE(scope->curve_name("gate/" + w.project().patterns.front().id), QString("Firing pulse"));
        if (auto screenshot = qEnvironmentVariable("PDS_THYRISTOR_SCREENSHOT"); !screenshot.isEmpty()) {
            QTest::qWait(30); QVERIFY(w.grab().save(screenshot));
            QVERIFY(graph->grab().save(screenshot + "-plot.png"));
        }
    }
    void pwl_device_properties_and_run() {
        QTemporaryDir dir;
        EditorWindow w("ru", dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR "/examples/diode-freewheel.pds")));
        ready(w);
        for (const auto kind : {Kind::diode, Kind::ideal_switch}) {
            const auto id = std::find_if(w.project().components.begin(), w.project().components.end(),
                                         [&](const auto &c) { return c.kind == kind; })->id;
            w.select_object(id);
            auto *mode = w.findChild<QComboBox *>("property_semiconductor_model");
            auto *ron = w.findChild<QLineEdit *>("property_ron");
            auto *roff = w.findChild<QLineEdit *>("property_roff");
            auto *vf = w.findChild<QLineEdit *>("property_forward_voltage");
            QVERIFY(mode && mode->isVisible());
            QVERIFY(!ron->isVisible());
            auto select_mode = [&](int index) {
                mode->setCurrentIndex(index);
                QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, index));
            };
            select_mode(1);
            QVERIFY(ron->isVisible() && roff->isVisible());
            QCOMPARE(vf->isVisible(), kind == Kind::diode);
            auto *charge = w.findChild<QComboBox *>("property_charge_model");
            auto *transit = w.findChild<QLineEdit *>("property_transit_time");
            auto *lifetime = w.findChild<QLineEdit *>("property_carrier_lifetime");
            auto *initial_charge = w.findChild<QLineEdit *>("property_initial_charge");
            QCOMPARE(charge->isVisible(), kind == Kind::diode);
            QVERIFY(!transit->isVisible());
            if (kind == Kind::diode) {
                charge->setCurrentIndex(1);
                QMetaObject::invokeMethod(charge, "activated", Q_ARG(int, 1));
                QVERIFY(transit->isVisible() && lifetime->isVisible() && initial_charge->isVisible());
                transit->setText("100 us"); QTest::keyClick(transit, Qt::Key_Return);
                lifetime->setText("500 us"); QTest::keyClick(lifetime, Qt::Key_Return);
                initial_charge->setText("1 uC"); QTest::keyClick(initial_charge, Qt::Key_Return);
            }
            const auto before = encoded(w.root_project());
            ron->setText("0"); QTest::keyClick(ron, Qt::Key_Return);
            QCOMPARE(encoded(w.root_project()), before);
            QVERIFY(!w.findChild<QLabel *>("property_error")->text().isEmpty());
            ron->setText("200 mOhm"); QTest::keyClick(ron, Qt::Key_Return);
            roff->setText("100 kOhm"); QTest::keyClick(roff, Qt::Key_Return);
            if (kind == Kind::diode) { vf->setText("800 mV"); QTest::keyClick(vf, Qt::Key_Return); }
            auto device = [&]() -> const Component & {
                return *std::find_if(w.project().components.begin(), w.project().components.end(),
                                     [&](const auto &c) { return c.id == id; });
            };
            QCOMPARE(device().semiconductor.ron, .2);
            QCOMPARE(device().semiconductor.roff, 1e5);
            if (kind == Kind::diode) QCOMPARE(device().semiconductor.forward_voltage, .8);
            if (kind == Kind::diode) {
                QCOMPARE(device().semiconductor.transit_time, 1e-4);
                QCOMPARE(device().semiconductor.carrier_lifetime, 5e-4);
                QCOMPARE(device().semiconductor.initial_charge, 1e-6);
                QVERIFY(device().semiconductor.charge_dynamics);
            }
            select_mode(0); QVERIFY(!ron->isVisible());
            w.undo(); QCOMPARE(device().semiconductor.model, SemiconductorModel::piecewise_linear);
            QVERIFY(ron->isVisible());
            if (kind == Kind::diode) QVERIFY(transit->isVisible() && device().semiconductor.charge_dynamics);
            if (auto path = qEnvironmentVariable("PDS_PWL_SCREENSHOT"); !path.isEmpty() && kind == Kind::diode) {
                QTest::qWait(30); QVERIFY(w.grab().save(path));
            }
        }
        w.start_simulation(); QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 2000);
        QVERIFY(w.has_result());
        const auto path = dir.filePath("pwl.pds");
        QVERIFY(w.save_project(path)); const auto expected = encoded(w.root_project());
        QVERIFY(w.open_project(path)); QCOMPARE(encoded(w.root_project()), expected);
    }
    void diode_recovery_example() {
        QTemporaryDir dir;
        EditorWindow w("ru", dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR "/examples/diode-recovery.pds")));
        ready(w);
        w.canvas()->fitInView(w.canvas()->scene()->itemsBoundingRect().adjusted(-60, -60, 60, 60), Qt::KeepAspectRatio);
        w.start_simulation(); QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 2000);
        QVERIFY(w.has_result() && !w.result().samples.empty());
        const auto plot = w.project().plots.front().id;
        w.open_plot(plot);
        auto *graph = w.findChild<QDialog *>("plot_" + QString::fromStdString(plot));
        QVERIFY(graph && graph->isVisible());
        if (auto path = qEnvironmentVariable("PDS_RECOVERY_SCREENSHOT"); !path.isEmpty()) {
            QTest::qWait(30);
            QVERIFY(w.grab().save(path));
            QVERIFY(graph->grab().save(path + "-plot.png"));
        }
    }
    void import_source_table() {
        struct RestoreNativeDialogs {
            bool previous = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
            ~RestoreNativeDialogs() { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, previous); }
        } restore;
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        QTemporaryDir dir;
        EditorWindow w("ru", dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR "/examples/rc-table.pds")));
        ready(w);
        const auto source = std::find_if(w.project().components.begin(), w.project().components.end(),
                                        [](const auto &c) { return c.kind == Kind::voltage; })->id;
        w.select_object(source);
        auto *button = w.findChild<QPushButton *>("import_source_points");
        auto *table = w.findChild<QTableWidget *>("property_source_points");
        QVERIFY(button && button->isVisible());
        auto choose = [&](const QString &path, bool cancel = false) {
            bool visited = false;
            QTimer timer;
            QElapsedTimer elapsed;
            elapsed.start();
            connect(&timer, &QTimer::timeout, &w, [&] {
                if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                    if (cancel || elapsed.elapsed() > 3000) {
                        visited |= cancel;
                        timer.stop();
                        dialog->reject();
                    } else if (!visited) {
                        visited = true;
                        dialog->setDirectory(QFileInfo(path).absolutePath());
                        dialog->selectFile(QFileInfo(path).fileName());
                        auto *edit = dialog->findChild<QLineEdit *>("fileNameEdit");
                        if (edit) edit->setText(QFileInfo(path).fileName());
                    } else {
                        QMetaObject::invokeMethod(dialog, "accept");
                    }
                } else if (elapsed.elapsed() > 3000) {
                    if (auto *other = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                        other->reject();
                }
            });
            timer.start(20);
            QTest::mouseClick(button, Qt::LeftButton);
            return visited;
        };
        auto file = dir.filePath("signal.csv");
        QFile out(file);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write("time[s];value[V]\n0;1,2345678901234567\n0,002;2,5\n");
        out.close();
        const auto before = encoded(w.root_project());
        QVERIFY(choose(file));
        QCOMPARE(table->rowCount(), 3);
        QCOMPARE(table->item(0, 1)->text().toDouble(), 1.2345678901234567);
        QCOMPARE(table->item(1, 0)->text().toDouble(), .002);
        QCOMPARE(encoded(w.root_project()), before); // Import is a draft until Apply.
        QVERIFY(choose({}, true));
        QCOMPARE(table->rowCount(), 3);
        const auto resistor = std::find_if(w.project().components.begin(), w.project().components.end(),
                                           [](const auto &c) { return c.kind == Kind::resistor; })->id;
        w.select_object(resistor);
        QVERIFY(!button->isVisible());
        w.select_object(source);
        QVERIFY(button->isVisible());
        QCOMPARE(table->item(1, 1)->text().toDouble(), 2.5); // Draft survives navigation.
        QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
        out.write("time,value\n0,1\n0,2\n"); out.close();
        QVERIFY(choose(file));
        QCOMPARE(table->item(1, 1)->text().toDouble(), 2.5);
        QVERIFY(w.findChild<QLabel *>("property_error")->text().contains("Line 3:"));
        QCOMPARE(encoded(w.root_project()), before);
        QTest::mouseClick(w.findChild<QPushButton *>("apply_properties"), Qt::LeftButton);
        QVERIFY(w.findChild<QLabel *>("property_error")->text().isEmpty());
        const auto applied = encoded(w.root_project());
        QVERIFY(applied != before);
        w.undo(); QCOMPARE(encoded(w.root_project()), before);
        w.redo(); QCOMPARE(encoded(w.root_project()), applied);
        QVERIFY(QFile::remove(file));
        const auto saved = dir.filePath("imported.pds");
        QVERIFY(w.save_project(saved)); QVERIFY(w.open_project(saved));
        QCOMPARE(encoded(w.root_project()), applied);
        w.start_simulation(); QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 2000);
        QVERIFY(w.has_result());
        if (auto path = qEnvironmentVariable("PDS_IMPORT_SCREENSHOT"); !path.isEmpty()) {
            w.select_object(source);
            QTest::qWait(30);
            QVERIFY(w.grab().save(path));
        }
    }
    void source_waveform_properties_and_run() {
        QTemporaryDir dir;
        EditorWindow w("ru",dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR "/examples/rc.pds")));
        ready(w);
        const auto source=std::find_if(w.project().components.begin(),w.project().components.end(),
            [](const auto &c){return c.kind==Kind::voltage;})->id;
        w.select_object(source);
        auto *mode=w.findChild<QComboBox*>("property_source_mode");
        QVERIFY(mode&&mode->isVisible());
        auto select_mode=[&](int index){mode->setCurrentIndex(index);QMetaObject::invokeMethod(mode,"activated",Q_ARG(int,index));};
        select_mode(1);
        auto *frequency=w.findChild<QLineEdit*>("property_source_frequency");
        QVERIFY(frequency->isVisible());
        frequency->setText("75 Hz");QTest::keyClick(frequency,Qt::Key_Return);
        auto selected_source=[&]() -> const Component& {
            return *std::find_if(w.project().components.begin(),w.project().components.end(),
                [&](const auto &c){return c.id==source;});
        };
        QCOMPARE(selected_source().source.kind,Waveform::sine);
        QCOMPARE(selected_source().source.frequency,75.);
        w.start_simulation();QTRY_VERIFY_WITH_TIMEOUT(!w.running(),2000);
        QVERIFY(w.has_result());
        w.select_object(source);
        select_mode(2);
        auto *duty=w.findChild<QLineEdit*>("property_source_duty");
        QVERIFY(duty->isVisible());
        duty->setText("25");QTest::keyClick(duty,Qt::Key_Return);
        QCOMPARE(selected_source().source.duty,.25);
        select_mode(3);
        auto *points=w.findChild<QTableWidget*>("property_source_points");
        QVERIFY(points->isVisible());QVERIFY(!frequency->isVisible());
        points->setItem(0,0,new QTableWidgetItem("0s"));points->setItem(0,1,new QTableWidgetItem("0V"));
        points->setItem(1,0,new QTableWidgetItem("2 ms"));points->setItem(1,1,new QTableWidgetItem("3 V"));
        QTest::mouseClick(w.findChild<QPushButton*>("apply_properties"),Qt::LeftButton);
        QCOMPARE(selected_source().source.points,(std::vector<Point>{{0,0},{.002,3}}));
        w.start_simulation();QTRY_VERIFY_WITH_TIMEOUT(!w.running(),2000);QVERIFY(w.has_result());
        if(auto path=qEnvironmentVariable("PDS_SOURCE_SCREENSHOT");!path.isEmpty())QVERIFY(w.grab().save(path));
        const auto saved=dir.filePath("source.pds");QVERIFY(w.save_project(saved));
        const auto expected=encoded(w.root_project());QVERIFY(w.open_project(saved));
        QCOMPARE(encoded(w.root_project()),expected);
        w.select_object(source);select_mode(0);QVERIFY(!points->isVisible());
        QCOMPARE(selected_source().source.kind,Waveform::dc);
        w.undo();QCOMPARE(selected_source().source.kind,Waveform::piecewise_linear);
        for(const auto *form:{"sine","pulse","table"}) {
            QVERIFY(w.open_project(QString(PDS_SOURCE_DIR "/examples/rc-%1.pds").arg(form)));
            const auto example_source=std::find_if(w.project().components.begin(),w.project().components.end(),
                [](const auto &c){return c.kind==Kind::voltage;})->id;
            for(auto *label:w.canvas()->scene()->items())
                if(label->data(1).toString()=="label"&&label->data(0).toString().toStdString()==example_source)
                    QVERIFY(!label->sceneBoundingRect().intersects(item(w,example_source)->mapRectToScene(QRectF(-28,-28,56,56))));
            auto reversed=w.project();
            for(auto &wire:reversed.wires) {
                if(wire.to.object!=reversed.plots.front().id)continue;
                QCOMPARE(static_cast<QGraphicsPathItem*>(item(w,wire.id))->pen().color(),QColor("#8c67c8"));
                std::swap(wire.from,wire.to);
                std::reverse(wire.bends.begin(),wire.bends.end());
            }
            w.set_project(reversed);
            for(const auto &wire:reversed.wires)
                if(wire.from.object==reversed.plots.front().id)
                    QCOMPARE(static_cast<QGraphicsPathItem*>(item(w,wire.id))->pen().color(),QColor("#8c67c8"));
            w.start_simulation();QTRY_VERIFY_WITH_TIMEOUT(!w.running(),2000);
            QVERIFY(w.has_result()&&!w.result().samples.empty());
            w.open_plot(w.project().plots.front().id);
            auto *graph=w.findChild<QDialog*>("plot_"+QString::fromStdString(w.project().plots.front().id));
            QVERIFY(graph&&graph->isVisible());
            if(auto path=qEnvironmentVariable("PDS_SOURCE_SCREENSHOT");!path.isEmpty()&&QString(form)=="sine") {
                QVERIFY(w.grab().save(path+"-example.png"));
                QVERIFY(graph->grab().save(path+"-plot.png"));
            }
        }
    }
    void dark_theme_editor_graphs_and_persistence() {
        QTemporaryDir dir;
        {
            EditorWindow w("ru", dir.path());
            QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/vsi-2l.pds"));
            ready(w);
            w.start_simulation();
            QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 10000);
            QVERIFY(w.has_result());
            const auto plot = w.project().plots.front().id;
            w.open_plot(plot);
            auto *graph = w.findChild<QDialog *>("plot_" + QString::fromStdString(plot));
            QVERIFY(graph);
            auto *scope = graph->findChild<Scope *>("scope");
            auto options = scope->view_options();
            options.legend = true;
            scope->load_view_options(options);
            scope->set_cursor_mode(true);
            scope->set_cursor(0, .0015);
            scope->set_cursor(1, .0045);
            const auto before = encoded(w.project());
            auto *action = w.findChild<QAction *>("dark_theme");
            QVERIFY(action && action->isCheckable());
            action->setChecked(true);
            QVERIFY(dark_theme());
            QCOMPARE(encoded(w.project()), before);
            QVERIFY(graph->palette().color(QPalette::Base).lightness() < 90);
            QVERIFY(graph->palette().color(QPalette::Text).lightness() > 200);
            const auto background = scope->grab().toImage().pixelColor(5, 5);
            QCOMPARE(background, theme_colors().surface);
            const auto key = plot_channels(w.project(), plot).front();
            scope->show_curve_settings(key);
            auto *settings = scope->findChild<QDialog *>("curve_settings");
            QVERIFY(settings && settings->isVisible());
            QVERIFY(settings->palette().color(QPalette::Window).lightness() < 90);
            if (auto folder = qEnvironmentVariable("PDS_THEME_SCREENSHOT_DIR"); !folder.isEmpty()) {
                w.canvas()->fitInView(w.canvas()->scene()->itemsBoundingRect().adjusted(-50, -50, 50, 50),
                                      Qt::KeepAspectRatio);
                QVERIFY(w.grab().save(folder + "/theme-dark-editor.png"));
                QVERIFY(graph->grab().save(folder + "/theme-dark-graph.png"));
                QVERIFY(settings->grab().save(folder + "/theme-dark-settings.png"));
            }
            settings->close();
            action->setChecked(false);
            QVERIFY(!dark_theme());
            QCOMPARE(scope->grab().toImage().pixelColor(5, 5), theme_colors().surface);
            action->setChecked(true);
            QCOMPARE(encoded(w.project()), before);
        }
        {
            EditorWindow reopened("en", dir.path());
            QVERIFY(dark_theme());
            QVERIFY(reopened.findChild<QAction *>("dark_theme")->isChecked());
            reopened.findChild<QAction *>("dark_theme")->setChecked(false);
        }
    }
    void editable_converter_examples() {
        QTemporaryDir dir;
        for (const auto &example : {QString("vsi-2l"), QString("npc-3l")}) {
            EditorWindow w("en", dir.path());
            QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/" + example + ".pds"));
            ready(w);
            const auto instance = w.project().instances.front().id;
            w.open_subcircuit(instance);
            QCOMPARE(w.project().instances.size(), size_t(3));
            const auto phases = w.project().instances;
            auto edit_every_atom = [&] {
                w.findChild<QAction *>("edit_definition")->trigger();
                const auto components = w.project().components;
                for (const auto &c : components) {
                    const auto before = encoded(w.root_project());
                    w.select_object(c.id);
                    QVERIFY(item(w, c.id)->isSelected());
                    auto *name = w.findChild<QLineEdit *>("property_name");
                    QVERIFY(name && !name->isReadOnly());
                    name->setText(QString::fromStdString(c.name) + " edited");
                    QTest::keyClick(name, Qt::Key_Return);
                    const auto changed = std::find_if(w.project().components.begin(), w.project().components.end(),
                        [&](const auto &candidate) { return candidate.id == c.id; });
                    QCOMPARE(changed->name, c.name + " edited");
                    w.undo();
                    QCOMPARE(encoded(w.root_project()), before);
                }
                // Public gate and neutral connections are ordinary selectable wires.
                for (const auto &wire : w.project().wires) {
                    w.select_object(wire.id);
                    QVERIFY(item(w, wire.id)->isSelected());
                }
            };
            edit_every_atom(); // DC-link capacitors, ESR and neutral wiring.
            for (const auto &phase : phases) {
                w.navigate_hierarchy({instance, phase.id});
                edit_every_atom(); // Every switch, diode, clamp and individual RC snubber.
            }
            w.navigate_hierarchy({instance, phases.front().id});
            const auto switches = example == "npc-3l" ? 4 : 2;
            QCOMPARE(std::count_if(w.project().components.begin(), w.project().components.end(),
                                   [](const auto &c) { return c.kind == Kind::ideal_switch; }),
                     switches);
            for (const auto &c : w.project().components) {
                w.select_object(c.id);
                QVERIFY(item(w, c.id)->isSelected());
            }
            w.findChild<QAction *>("edit_definition")->trigger();
            auto component =
                std::find_if(w.project().components.begin(), w.project().components.end(), [](const auto &c) {
                    return c.name == "Rsn1";
                })->id;
            w.select_object(component);
            auto *value = w.findChild<QLineEdit *>("property_value");
            QVERIFY(!value->isReadOnly());
            value->setText("120 Ohm");
            QTest::keyClick(value, Qt::Key_Return);
            w.start_simulation();
            QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 10000);
            QVERIFY(w.has_result() && !w.result().samples.empty());
            if (auto folder = qEnvironmentVariable("PDS_CONVERTER_SCREENSHOT_DIR"); !folder.isEmpty()) {
                w.canvas()->fitInView(w.canvas()->scene()->itemsBoundingRect().adjusted(-50, -50, 50, 50),
                                      Qt::KeepAspectRatio);
                QVERIFY(w.grab().save(folder + "/" + example + "-leg.png"));
            }
            w.navigate_hierarchy({});
            w.canvas()->fitInView(w.canvas()->scene()->itemsBoundingRect().adjusted(-50, -50, 50, 50),
                                  Qt::KeepAspectRatio);
            if (auto folder = qEnvironmentVariable("PDS_CONVERTER_SCREENSHOT_DIR"); !folder.isEmpty())
                QVERIFY(w.grab().save(folder + "/" + example + "-root.png"));
            w.open_plot(w.project().plots.front().id);
            auto *graph =
                w.findChild<QDialog *>("plot_" + QString::fromStdString(w.project().plots.front().id));
            QVERIFY(graph && graph->isVisible());
            QVERIFY(graph->findChild<QPushButton *>("plot_export")->isEnabled());
            QVERIFY(w.save_project(dir.filePath(example + ".pds")));
        }
    }
    void nested_topology_error_navigation() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR "/examples/vsi-2l.pds")));
        auto p = w.project();
        const auto converter = p.instances.front();
        const auto leg = definition(p, converter.definition).instances.front();
        auto &body = *std::find_if(p.definitions.begin(), p.definitions.end(),
            [&](const auto &d) { return d.id == leg.definition; });
        const auto target = body.components.front().id;
        for (int value : {1, 2}) {
            const auto id = new_uuid();
            body.components.push_back({id, "Conflict", Kind::voltage, "", "", double(value)});
            body.wires.push_back({new_uuid(), {id, "p"}, {target, "p"}, {}});
            body.wires.push_back({new_uuid(), {id, "n"}, {target, "n"}, {}});
        }
        w.set_project(p);
        ready(w);
        w.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 2000);
        QVERIFY(!w.has_result());
        auto *diagnostics = w.findChild<QListWidget *>("diagnostics_list");
        QCOMPARE(diagnostics->count(), 1);
        QVERIFY(diagnostics->item(0)->text().contains("conflicting_voltage_constraints"));
        const auto object = diagnostics->item(0)->data(Qt::UserRole).toString().toStdString();
        QTest::mouseClick(diagnostics->viewport(), Qt::LeftButton, Qt::NoModifier,
            diagnostics->visualItemRect(diagnostics->item(0)).center());
        QCOMPARE(w.hierarchy_path(), (std::vector<std::string>{converter.id, leg.id}));
        QVERIFY(item(w, object) && item(w, object)->isSelected());
    }
    void hierarchy_graph_windows_remain_independent() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        ready(w);
        auto p = w.project();
        const auto graph = new_uuid();
        p.plots.push_back({graph, "Output", 500, 0, 1});
        p.wires.push_back({new_uuid(), {p.nodes[2].id, "node"}, {graph, "in1"}, {}});
        Document document(p);
        const auto first = document.create_definition({graph}, "Scope cell");
        auto duplicate = document.paste(document.copy({first}), 0, 200);
        const auto second = duplicate.front();
        w.set_project(document.root_project());
        w.open_subcircuit(first);
        w.open_plot(graph);
        const auto first_id = expanded_uuid({first}, graph), second_id = expanded_uuid({second}, graph);
        auto *first_window = w.findChild<QDialog *>("plot_" + QString::fromStdString(first_id));
        QVERIFY(first_window);
        auto *first_scope = first_window->findChild<Scope *>("scope");
        QVERIFY(first_scope);
        first_scope->changed(.0001, .0004, .0002, .0003);
        w.navigate_hierarchy({});
        QVERIFY(first_window->isVisible());
        w.open_subcircuit(second);
        w.open_plot(graph);
        auto *second_window = w.findChild<QDialog *>("plot_" + QString::fromStdString(second_id));
        QVERIFY(second_window && second_window != first_window);
        second_window->findChild<Scope *>("scope")->changed(.0005, .0009, .0006, .0008);
        w.navigate_hierarchy({});
        QVERIFY(first_window->isVisible() && second_window->isVisible());
        QCOMPARE(w.root_project().view_options.size(), size_t(2));
        QCOMPARE(w.root_project().view_options[0].begin, .0001);
        QCOMPARE(w.root_project().view_options[1].begin, .0005);
        w.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 10000);
        QVERIFY(w.has_result());
        QVERIFY(first_window->findChild<QPushButton *>("plot_export")->isEnabled());
        QVERIFY(!second_window->findChild<QPushButton *>("plot_export")->isEnabled());
        QVERIFY(w.save_project(dir.filePath("views.pds")));
    }
    void hierarchy_edit_run_save_and_undo() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        ready(w);
        auto initial = w.project();
        initial.scope_enabled = true;
        initial.scope_channels = {initial.nodes[2].id};
        w.set_project(initial);
        const auto resistor = initial.components[1].id, capacitor = initial.components[2].id,
                   output = initial.nodes[2].id;
        for (const auto &id : {resistor, capacitor, output})
            item(w, id)->setSelected(true);
        const auto grouped = w.group_selection("RC cell");
        QVERIFY(!grouped.empty());
        QCOMPARE(w.root_project().instances.size(), size_t(1));
        QVERIFY(item(w, grouped));
        const auto shared = w.root_project().instances.front().definition;
        w.canvas()->resetTransform();
        w.canvas()->centerOn(item(w, grouped)->pos());
        QTest::mouseDClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                           w.canvas()->mapFromScene(item(w, grouped)->pos()));
        QCOMPARE(w.hierarchy_path(), std::vector<std::string>{grouped});
        QCOMPARE(w.project().components.size(), size_t(2));
        w.select_object(resistor);
        auto *value = w.findChild<QLineEdit *>("property_value");
        QVERIFY(value->isReadOnly());
        w.findChild<QAction *>("edit_definition")->trigger();
        QVERIFY(!value->isReadOnly());
        value->setFocus();
        value->selectAll();
        QTest::keyClicks(value, "2 kOhm");
        QTest::keyClick(value, Qt::Key_Return);
        QCOMPARE(definition(w.root_project(), shared).components.front().value, 2000.);
        w.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 10000);
        QVERIFY(w.has_result());
        QVERIFY(!w.result().samples.empty());
        QCOMPARE(w.result().project_id, w.root_project().id);
        const auto path = dir.filePath("hierarchical.pds");
        QVERIFY(w.save_project(path));
        QFile saved(path);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        std::istringstream data(saved.readAll().toStdString());
        auto loaded = read_project(data);
        QCOMPARE(loaded, w.root_project());
        QVERIFY(loaded.instances.size() == 1 && loaded.components.size() == 1);
        w.findChild<QAction *>("hierarchy_up")->trigger();
        QVERIFY(w.hierarchy_path().empty());
        w.undo();
        QCOMPARE(w.hierarchy_path(), std::vector<std::string>{grouped});
        w.redo();
        QVERIFY(w.hierarchy_path().empty());
        w.select_object(grouped);
        w.detach_selected();
        QVERIFY(w.root_project().instances.front().definition != shared);
        w.open_subcircuit(grouped);
        w.select_object(resistor);
        w.findChild<QAction *>("edit_definition")->trigger();
        value = w.findChild<QLineEdit *>("property_value");
        value->setFocus();
        value->selectAll();
        QTest::keyClicks(value, "3 kOhm");
        QTest::keyClick(value, Qt::Key_Return);
        QCOMPARE(definition(w.root_project(), shared).components.front().value, 2000.);
        w.navigate_hierarchy({});
        w.select_object(grouped);
        w.expand_selected();
        QVERIFY(w.root_project().instances.empty());
        QCOMPARE(w.project().components.size(), size_t(3));
        w.undo();
        QCOMPARE(w.project().instances.size(), size_t(1));
        if (auto screenshot = qEnvironmentVariable("PDS_HIERARCHY_SCREENSHOT_PATH"); !screenshot.isEmpty()) {
            w.canvas()->fitInView(w.canvas()->scene()->itemsBoundingRect().adjusted(-80, -80, 80, 80),
                                  Qt::KeepAspectRatio);
            QVERIFY(w.grab().save(screenshot));
        }
    }
    void public_interface_and_instance_parameters() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        ready(w);
        const auto resistor = w.project().components[1].id;
        w.select_object(resistor);
        auto group = w.group_selection("R cell");
        QVERIFY(!group.empty());
        bool handled = false;
        QTimer::singleShot(20, &w, [&] {
            auto *dialog = w.findChild<QDialog *>("public_interface_dialog");
            if (!dialog)
                return;
            auto *ports = dialog->findChild<QTableWidget *>("public_ports");
            ports->item(0, 0)->setText("positive");
            ports->item(1, 0)->setText("negative");
            auto *parameters = dialog->findChild<QTableWidget *>("public_parameters");
            dialog->findChild<QPushButton *>("public_parameters_add")->click();
            parameters->item(0, 0)->setText("Resistance");
            qobject_cast<QLineEdit *>(parameters->cellWidget(0, 2))->setText("2 kOhm");
            handled = true;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        // Ensure a regression cannot leave an unattended modal window hanging.
        QTimer::singleShot(2000, &w, [&] {
            if (auto *dialog = w.findChild<QDialog *>("public_interface_dialog"))
                dialog->reject();
        });
        w.findChild<QAction *>("public_interface")->trigger();
        QVERIFY(handled);
        auto definition_id = w.project().instances.front().definition;
        const auto &d = definition(w.root_project(), definition_id);
        QCOMPARE(d.ports[0].name, std::string("positive"));
        QCOMPARE(d.parameters.size(), size_t(1));
        QCOMPARE(d.parameters[0].value, 2000.);
        auto *parameter =
            w.findChild<QLineEdit *>("property_parameter/" + QString::fromStdString(d.parameters[0].id));
        QVERIFY(parameter && parameter->isVisible());
        parameter->setFocus();
        parameter->selectAll();
        QTest::keyClicks(parameter, "3 kOhm");
        QTest::keyClick(parameter, Qt::Key_Return);
        QCOMPARE(w.project().instances.front().parameters.front().second, 3000.);
        QCOMPARE(definition(w.root_project(), definition_id).parameters[0].value, 2000.);
        w.open_subcircuit(group);
        w.select_object(resistor);
        w.findChild<QAction *>("edit_definition")->trigger();
        auto *value = w.findChild<QLineEdit *>("property_value");
        value->setFocus();
        value->selectAll();
        QTest::keyClicks(value, "4 kOhm");
        QTest::keyClick(value, Qt::Key_Return);
        QCOMPARE(definition(w.root_project(), definition_id).parameters[0].value, 4000.);
        auto flat = flatten(w.root_project()).project;
        auto component = std::find_if(flat.components.begin(), flat.components.end(), [&](const auto &c) {
            return c.id == expanded_uuid({group}, resistor);
        });
        QVERIFY(component != flat.components.end());
        QCOMPARE(component->value, 3000.);
    }
    void decimal_comma_in_scalar_fields() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        const auto resistor = w.add_component(Kind::resistor, {0, 0});
        ready(w);
        auto *stop = w.findChild<QLineEdit *>("sim_stop");
        stop->setText("12");
        stop->setCursorPosition(1);
        QTest::keyClick(stop, Qt::Key_Comma);
        QCOMPARE(stop->text(), QString("1.2"));
        QCOMPARE(stop->cursorPosition(), 2);
        stop->undo();
        QCOMPARE(stop->text(), QString("12"));
        stop->redo();
        QCOMPARE(stop->text(), QString("1.2"));
        stop->selectAll();
        QTest::keyClicks(stop, "1,25e-2");
        QCOMPARE(stop->text(), QString("1.25e-2"));
        QTest::keyClick(stop, Qt::Key_Return);
        QCOMPARE(w.project().profile.stop, .0125);
        auto *step = w.findChild<QLineEdit *>("sim_step");
        step->selectAll();
        QTest::keyClicks(step, "2,5 us");
        QCOMPARE(step->text(), QString("2.5 us"));
        QTest::keyClick(step, Qt::Key_Return);
        QVERIFY(std::abs(w.project().profile.step - 2.5e-6) < 1e-15);
        w.select_object(resistor);
        auto *value = w.findChild<QLineEdit *>("property_value");
        // Paste exercises the same normalization path, including units and selection replacement.
        auto *clipboard = new QMimeData;
        if (const auto *original = QApplication::clipboard()->mimeData())
            for (const auto &format : original->formats())
                clipboard->setData(format, original->data(format));
        QApplication::clipboard()->setText("1,5 kOhm");
        value->selectAll();
        QTest::keyClick(value, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(value->text(), QString("1.5 kOhm"));
        QApplication::clipboard()->setText("2,5 kOhm");
        value->setFocus();
        value->selectAll();
        QTimer::singleShot(0, value, [] {
            if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                if (auto *paste = menu->findChild<QAction *>("paste_number"))
                    paste->trigger();
                menu->close();
            }
        });
        QContextMenuEvent context(QContextMenuEvent::Keyboard, QPoint(5, 5),
                                  value->mapToGlobal(QPoint(5, 5)));
        QApplication::sendEvent(value, &context);
        QApplication::clipboard()->setMimeData(clipboard);
        QCOMPARE(value->text(), QString("2.5 kOhm"));
        QTest::keyClick(value, Qt::Key_Return);
        QCOMPARE(w.project().components.front().value, 2500.);
        auto *name = w.findChild<QLineEdit *>("property_name");
        name->selectAll();
        QTest::keyClicks(name, "R, load");
        QCOMPARE(name->text(), QString("R, load"));
        QTest::keyClick(name, Qt::Key_Return);
        auto *search = w.findChild<QLineEdit *>("library_search");
        QTest::keyClicks(search, "R,C");
        QCOMPARE(search->text(), QString("R,C"));
        search->clear();
        auto *points = w.findChild<QPlainTextEdit *>("property_bends");
        QVERIFY(points);
        points->insertPlainText("1,2 3,4");
        QCOMPARE(points->toPlainText(), QString("1,2 3,4"));
        QTest::mouseDClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                           w.canvas()->mapFromScene(QPointF(0, -41)));
        auto *inline_value = w.findChild<QLineEdit *>("inline_property");
        QVERIFY(inline_value);
        inline_value->selectAll();
        QTest::keyClicks(inline_value, "2,2 kOhm");
        QCOMPARE(inline_value->text(), QString("2.2 kOhm"));
        QTest::keyClick(inline_value, Qt::Key_Return);
        QCOMPARE(w.project().components.front().value, 2200.);
        const auto pattern = w.add_pattern({200, 0});
        w.select_object(pattern);
        auto *events = w.findChild<QTableWidget *>("property_events");
        QVERIFY(events);
        events->setCurrentCell(0, 0);
        events->editItem(events->item(0, 0));
        auto *time = events->findChild<QLineEdit *>();
        QVERIFY(time && events->isAncestorOf(time));
        time->selectAll();
        QTest::keyClicks(time, "0,5s");
        QCOMPARE(time->text(), QString("0.5s"));
        QTest::keyClick(time, Qt::Key_Tab);
        QCOMPARE(events->item(0, 0)->text(), QString("0.5s"));
        Scope scope;
        auto *nav = scope.navigation();
        auto *span = nav->findChild<QLineEdit *>("scope_time_span");
        span->selectAll();
        QTest::keyClicks(span, "0,1 s");
        QCOMPARE(span->text(), QString("0.1 s"));
        QTest::keyClick(span, Qt::Key_Return);
        QCOMPARE(scope.view_options().time_span, .1);
        auto *cursor = nav->findChild<QLineEdit *>("cursor_time_a");
        QTest::keyClicks(cursor, "1,25e-3");
        QCOMPARE(cursor->text(), QString("1.25e-3"));
        scope.show_display_settings();
        auto *axis = scope.findChild<QLineEdit *>("y_min");
        axis->selectAll();
        QTest::keyClicks(axis, "-1,5");
        QCOMPARE(axis->text(), QString("-1.5"));
        delete nav;
    }
    void plot_trace_visibility() {
        init_language("ru");
        Result r;
        r.channels = {{"a", "Ток нагрузки", "A"}, {"b", "Ток источника", "A"}};
        for (int i = 0; i <= 100; ++i)
            r.samples.push_back({i * .01, {std::sin(i * .1), 100}, {}});
        Project p;
        p.scope_end = 1;
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        auto *scope = new Scope;
        auto *legend = qobject_cast<QToolBar *>(scope->channel_controls());
        layout->addWidget(legend);
        layout->addWidget(scope->navigation());
        layout->addWidget(scope, 1);
        host.resize(900, 470);
        scope->set_result(&r, {0, 1}, p);
        host.show();
        QTest::qWait(30);
        QCOMPARE(legend->actions().size(), 2);
        auto count_color = [&](QColor color) {
            const auto image = scope->grab().toImage();
            int count = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    const auto pixel = image.pixelColor(x, y);
                    count += std::abs(pixel.red() - color.red()) < 20 &&
                             std::abs(pixel.green() - color.green()) < 20 &&
                             std::abs(pixel.blue() - color.blue()) < 20;
                }
            return count;
        };
        QVERIFY(count_color(QColor("#c56819")) > 100);
        auto *b = legend->actions()[1];
        const double before_low = scope->y_low, before_high = scope->y_high;
        QTest::mouseClick(legend->widgetForAction(b)->findChild<QToolButton *>("curve_visibility"),
                          Qt::LeftButton, Qt::NoModifier, QPoint(10, 10));
        QVERIFY(!scope->channel_visible("b"));
        QVERIFY(!b->isChecked());
        QVERIFY(b->font().strikeOut());
        QCOMPARE(scope->y_low, before_low);
        QCOMPARE(scope->y_high, before_high);
        QCOMPARE(count_color(QColor("#c56819")), 0);
        QVERIFY(count_color(QColor("#146cca")) > 100);
        scope->fit(Scope::Axes::y);
        QVERIFY(scope->y_high < 2);
        QCOMPARE(r.samples.size(), size_t(101));
        QCOMPARE(r.channels.size(), size_t(2));
        scope->set_live(true);
        r.samples.push_back({1.1, {.5, 200}, {}});
        scope->set_result(&r, {0, 1}, p);
        QVERIFY(!scope->channel_visible("b"));
        QVERIFY(scope->y_high < 2);
        Scope restored;
        restored.load_view_options(scope->view_options());
        restored.set_result(&r, {0, 1}, p);
        QVERIFY(!restored.channel_visible("b"));
        QTest::mouseClick(legend->widgetForAction(b)->findChild<QToolButton *>("curve_visibility"),
                          Qt::LeftButton, Qt::NoModifier, QPoint(10, 10));
        QVERIFY(scope->channel_visible("b"));
        QVERIFY(!b->font().strikeOut());
        scope->fit(Scope::Axes::y);
        QVERIFY(scope->y_high > 200);
        QVERIFY(count_color(QColor("#c56819")) > 100);
        legend->actions()[0]->trigger();
        b->trigger();
        QCOMPARE(count_color(QColor("#146cca")), 0);
        QCOMPARE(count_color(QColor("#c56819")), 0);
        legend->actions()[0]->trigger();
        scope->fit(Scope::Axes::y);
        const auto screenshot = qEnvironmentVariable("PDS_VISIBILITY_SCREENSHOT_PATH");
        if (!screenshot.isEmpty())
            QVERIFY(host.grab().save(screenshot));
    }
    void curve_styles_and_movable_legend() {
        init_language("ru");
        Result result;
        result.channels = {{"a", "Напряжение", "V"}, {"b", "Ток", "A"}};
        for (int i = 0; i <= 40; ++i)
            result.samples.push_back({i * .025, {std::sin(i * .15), .4}, {}});
        Project project;
        project.scope_end = 1;
        QDialog host;
        auto *layout = new QVBoxLayout(&host);
        auto *export_button = new QPushButton("Export", &host);
        export_button->setDefault(true);
        layout->addWidget(export_button);
        int export_clicks = 0;
        connect(export_button, &QPushButton::clicked, &host, [&] { ++export_clicks; });
        auto *scope = new Scope;
        auto *bar = qobject_cast<QToolBar *>(scope->channel_controls());
        layout->addWidget(bar);
        layout->addWidget(scope->navigation());
        layout->addWidget(scope);
        ViewOptions options;
        options.legend = true;
        scope->load_view_options(options);
        scope->set_result(&result, {0, 1}, project);
        host.resize(900, 520);
        host.show();
        QTest::qWait(30);
        auto *gear = bar->findChild<QAction *>("curve_style_a");
        QVERIFY(gear);
        auto *group = bar->widgetForAction(bar->actions().front());
        auto *name_button = group->findChild<QToolButton *>("curve_visibility");
        const auto original_name = scope->curve_name("a");
        QTest::mouseClick(name_button, Qt::LeftButton, Qt::NoModifier, QPoint(50, 10));
        QTest::mouseDClick(name_button, Qt::LeftButton, Qt::NoModifier, QPoint(50, 10));
        auto *name_edit = group->findChild<QLineEdit *>("curve_name_edit");
        QVERIFY(name_edit->isVisible());
        QTest::keyClicks(name_edit, "Load voltage");
        QTest::keyClick(name_edit, Qt::Key_Return);
        QTest::qWait(QApplication::doubleClickInterval() + 20);
        QCOMPARE(scope->curve_name("a"), QString("Load voltage"));
        QCOMPARE(export_clicks, 0);
        QVERIFY(scope->channel_visible("a"));
        QVERIFY(bar->actions().front()->text().startsWith("Load voltage"));
        QTest::mouseDClick(name_button, Qt::LeftButton, Qt::NoModifier, QPoint(50, 10));
        QTest::keyClicks(name_edit, "Cancelled");
        QTest::keyClick(name_edit, Qt::Key_Escape);
        QCOMPARE(scope->curve_name("a"), QString("Load voltage"));
        QTest::mouseDClick(name_button, Qt::LeftButton, Qt::NoModifier, QPoint(50, 10));
        QTest::keyClick(name_edit, Qt::Key_Enter);
        QCOMPARE(export_clicks, 0);
        Scope named_copy;
        named_copy.load_view_options(scope->view_options());
        named_copy.set_result(&result, {0, 1}, project);
        QCOMPARE(named_copy.curve_name("a"), QString("Load voltage"));
        QVERIFY(original_name != scope->curve_name("a"));
        QTest::mouseClick(name_button, Qt::LeftButton, Qt::NoModifier, QPoint(50, 10));
        QTRY_VERIFY(!scope->channel_visible("a"));
        QTest::mouseClick(name_button, Qt::LeftButton, Qt::NoModifier, QPoint(10, 10));
        QVERIFY(scope->channel_visible("a"));
        QVERIFY(group->isAncestorOf(qobject_cast<QWidget *>(gear->parent())) || group == gear->parent());
        QToolButton *gear_button = nullptr;
        for (auto *button : group->findChildren<QToolButton *>())
            if (button->defaultAction() == gear)
                gear_button = button;
        QVERIFY(gear_button);
        QVERIFY(group->rect().contains(gear_button->geometry()));
        QTest::mouseClick(gear_button, Qt::LeftButton);
        QVERIFY(scope->channel_visible("a"));
        auto *dialog = scope->findChild<QDialog *>("curve_settings");
        QVERIFY(dialog);
        QCOMPARE(dialog->property("channel").toString(), QString("a"));
        dialog->findChild<QComboBox *>("curve_line")->setCurrentIndex(int(CurveLine::dash));
        dialog->findChild<QComboBox *>("curve_marker")->setCurrentIndex(int(CurveMarker::triangle));
        dialog->findChild<QLineEdit *>("curve_width")->setText("3");
        dialog->findChild<QLineEdit *>("curve_marker_size")->setText("9");
        QTest::mouseClick(dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply),
                          Qt::LeftButton);
        QCOMPARE(scope->curve_style("a").line, CurveLine::dash);
        QCOMPARE(scope->curve_style("a").marker, CurveMarker::triangle);
        QCOMPARE(scope->curve_style("b").line, CurveLine::solid);
        dialog->close();
        QTest::qWait(20);
        host.activateWindow();
        scope->setFocus();
        const double before_begin = scope->begin, before_end = scope->end;
        const auto follow = host.findChild<QAction *>("follow_live");
        QVERIFY(follow->isChecked());
        const QPoint start(100, 30), destination(440, 150);
        QTest::mousePress(scope, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(scope, destination);
        QTest::mouseRelease(scope, Qt::LeftButton, Qt::NoModifier, destination);
        auto saved = scope->view_options();
        QCOMPARE(saved.legend_positions.size(), size_t(1));
        QVERIFY(saved.legend_positions[0].x > .2 && saved.legend_positions[0].y > .2);
        QCOMPARE(scope->begin, before_begin);
        QCOMPARE(scope->end, before_end);
        QVERIFY(follow->isChecked());
        QTest::mousePress(scope, Qt::LeftButton, Qt::NoModifier, destination);
        QTest::mouseMove(scope, destination + QPoint(70, 30));
        QTest::keyClick(scope, Qt::Key_Escape);
        QTest::mouseRelease(scope, Qt::LeftButton, Qt::NoModifier, destination + QPoint(70, 30));
        QVERIFY(scope->view_options().legend_positions == saved.legend_positions);
        QVERIFY(follow->isChecked());
        Scope restored;
        restored.load_view_options(saved);
        restored.set_result(&result, {0, 1}, project);
        QVERIFY(restored.view_options().curve_styles == saved.curve_styles);
        QVERIFY(restored.view_options().legend_positions == saved.legend_positions);
        const auto screenshot = qEnvironmentVariable("PDS_STYLE_SCREENSHOT_PATH");
        if (!screenshot.isEmpty())
            QVERIFY(host.grab().save(screenshot));
        // With no line, every recorded sample is still represented by a marker.
        options.legend = false;
        scope->load_view_options(options);
        scope->set_channel_visible("b", false);
        scope->set_curve_style({"a", CurveLine::none, 2, CurveMarker::square, 8});
        scope->fit(Scope::Axes::xy);
        const auto image = scope->grab().toImage();
        const double scale = scope->devicePixelRatioF();
        for (int i = 2; i < 39; ++i) {
            const int x = int((58 + result.samples[i].time * (scope->width() - 84)) * scale);
            const int y = int((12 + (scope->y_high - result.samples[i].values[0]) /
                                        (scope->y_high - scope->y_low) * (scope->height() - 64)) *
                              scale);
            bool colored = false;
            for (int dy = -6 * int(std::ceil(scale)); dy <= 6 * int(std::ceil(scale)); ++dy)
                for (int dx = -6 * int(std::ceil(scale)); dx <= 6 * int(std::ceil(scale)); ++dx) {
                    const QColor c = image.pixelColor(x + dx, y + dy);
                    if (c.blue() > 140 && c.red() < 70 && c.green() < 160)
                        colored = true;
                }
            QVERIFY(colored);
        }
        QVERIFY(!QIcon(":/icons/application.png").isNull());
    }
    void scope_pan_and_sliding_window() {
        init_language("en");
        Result r;
        r.channels = {{"a", "A", "V"}};
        for (int i = 0; i <= 200; ++i)
            r.samples.push_back({i * .001, {std::sin(i * .1)}, {}});
        Project p;
        p.scope_end = .2;
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        auto *s = new Scope;
        auto *nav = s->navigation();
        layout->addWidget(nav);
        layout->addWidget(s);
        host.resize(850, 430);
        host.show();
        QTest::qWait(30);
        s->set_result(&r, {0}, p);
        s->changed = [&](double a, double b, double ca, double cb) {
            p.scope_begin = a;
            p.scope_end = b;
            p.cursor_a = ca;
            p.cursor_b = cb;
        };
        auto *bar = nav->findChild<QToolBar *>("plot_navigation");
        auto *zoom = nav->findChild<QAction *>("zoom_x");
        QVERIFY(zoom->isChecked());
        // Exercise the actual toolbar button, including QActionGroup's optional selection.
        QTest::mouseClick(bar->widgetForAction(zoom), Qt::LeftButton);
        QVERIFY(!zoom->isChecked());
        QCOMPARE(s->cursor().shape(), Qt::OpenHandCursor);
        const double width = s->end - s->begin, height = s->y_high - s->y_low;
        const double low = s->y_low;
        QTest::mousePress(s, Qt::LeftButton, Qt::NoModifier, {400, 140});
        QTest::mouseMove(s, {300, 180});
        QCOMPARE(s->cursor().shape(), Qt::ClosedHandCursor);
        QTest::mouseRelease(s, Qt::LeftButton, Qt::NoModifier, {300, 180});
        QVERIFY(s->begin > 0);
        QVERIFY(s->y_low > low);
        QVERIFY(std::abs(s->end - s->begin - width) < 1e-12);
        QVERIFY(std::abs(s->y_high - s->y_low - height) < 1e-12);
        QCOMPARE(s->cursor_a, -1.);
        const double begin = s->begin, end = s->end, pan_low = s->y_low;
        QTest::mousePress(s, Qt::LeftButton, Qt::NoModifier, {300, 180});
        QTest::mouseMove(s, {200, 100});
        QTest::keyClick(s, Qt::Key_Escape);
        QTest::mouseRelease(s, Qt::LeftButton, Qt::NoModifier, {200, 100});
        QCOMPARE(s->begin, begin);
        QCOMPARE(s->end, end);
        QCOMPARE(s->y_low, pan_low);
        auto *span = nav->findChild<QLineEdit *>("scope_time_span");
        span->setText("100 ms");
        QTest::keyClick(span, Qt::Key_Return);
        QCOMPARE(s->view_options().time_span, .1);
        QVERIFY(std::abs(s->begin - .1) < 1e-12);
        QCOMPARE(s->end, .2);
        s->set_live(true);
        r.samples.push_back({.3, {1}, {}});
        s->set_result(&r, {0}, p);
        QVERIFY(std::abs(s->begin - .2) < 1e-12);
        QCOMPARE(s->end, .3);
        QTest::mousePress(s, Qt::LeftButton, Qt::NoModifier, {300, 180});
        QTest::mouseRelease(s, Qt::LeftButton, Qt::NoModifier, {400, 180});
        const double fixed_end = s->end;
        r.samples.push_back({.4, {0}, {}});
        s->set_result(&r, {0}, p);
        QCOMPARE(s->end, fixed_end);
        nav->findChild<QAction *>("follow_live")->trigger();
        QCOMPARE(s->end, .4);
        QVERIFY(std::abs(s->end - s->begin - .1) < 1e-12);
        span->setText("-1 ms");
        QTest::keyClick(span, Qt::Key_Return);
        QCOMPARE(s->view_options().time_span, .1);
        span->setText("0");
        QTest::keyClick(span, Qt::Key_Return);
        QCOMPARE(s->begin, 0.);
        QCOMPARE(s->end, .4);
        s->set_time_span(.1);
        s->set_live(false);
        nav->findChild<QAction *>("cursor_mode")->trigger();
        QVERIFY(nav->findChild<QAction *>("cursor_mode")->isChecked());
        nav->findChild<QAction *>("cursor_mode")->trigger();
        QCOMPARE(s->cursor().shape(), Qt::OpenHandCursor);
        QTest::mouseClick(bar->widgetForAction(zoom), Qt::LeftButton);
        QVERIFY(zoom->isChecked());
        QTest::mousePress(s, Qt::LeftButton, Qt::NoModifier, {200, 100});
        QTest::mouseRelease(s, Qt::LeftButton, Qt::NoModifier, {500, 200});
        QVERIFY(s->end - s->begin < .05);
        Scope restored;
        restored.load_view_options(s->view_options());
        restored.set_time_span(.075);
        restored.set_live(true);
        restored.set_result(&r, {0}, p);
        QVERIFY(std::abs(restored.end - restored.begin - .075) < 1e-12);
        QCOMPARE(restored.view_options().time_span, .075);
        restored.set_time_span(.1);
        Result initial;
        initial.channels = r.channels;
        initial.samples = {{0, {0}, {}}, {.02, {1}, {}}};
        restored.set_result(&initial, {0}, p);
        QCOMPARE(restored.begin, 0.);
        QCOMPARE(restored.end, .1);
        auto screenshot = qEnvironmentVariable("PDS_SCOPE_TOOLBAR_SCREENSHOT_PATH");
        if (!screenshot.isEmpty())
            QVERIFY(host.grab().save(screenshot));
    }
    void channel_source_preview() {
        QTemporaryDir dir;
        EditorWindow w("ru", dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/diode-freewheel.pds"));
        w.set_scope_enabled(true);
        ready(w);
        w.findChild<QTabWidget *>("results_tabs")->setCurrentIndex(1);
        auto *list = w.findChild<QListWidget *>("channels");
        QVERIFY(list && list->count() > 0);
        auto click_channel = [&](const std::string &key) {
            for (int i = 0; i < list->count(); ++i) {
                auto *entry = list->item(i);
                if (entry->data(Qt::UserRole).toString().toStdString() == key) {
                    list->scrollToItem(entry);
                    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                                      list->visualItemRect(entry).center());
                    return true;
                }
            }
            return false;
        };
        const auto before = encoded(w.project());
        const auto resistor = std::find_if(w.project().components.begin(), w.project().components.end(),
                                           [](const Component &c) { return c.kind == Kind::resistor; })
                                  ->id;
        QVERIFY(click_channel(resistor));
        QVERIFY(item(w, resistor)->data(channel_highlight_role).toBool());
        QVERIFY(!item(w, resistor)->isSelected());
        QCOMPARE(encoded(w.project()), before);
        const auto graph = resolve_connections(w.project());
        const auto net = graph.nets.at(endpoint_key({resistor, "p"}));
        QVERIFY(click_channel(net));
        QVERIFY(!item(w, resistor)->data(channel_highlight_role).toBool());
        int highlighted = 0;
        for (const auto &wire : w.project().wires) {
            auto from = graph.nets.find(endpoint_key(wire.from));
            bool expected = from != graph.nets.end() && from->second == net;
            QCOMPARE(item(w, wire.id)->data(channel_highlight_role).toBool(), expected);
            highlighted += expected;
        }
        QVERIFY(highlighted > 0);
        const auto sw = std::find_if(w.project().components.begin(), w.project().components.end(),
                                     [](const Component &c) { return c.kind == Kind::ideal_switch; })
                            ->id;
        QVERIFY(click_channel("gate/" + sw));
        QVERIFY(item(w, sw)->data(channel_highlight_role).toBool());
        QCOMPARE(encoded(w.project()), before);
        auto *run = w.findChild<QToolButton *>("run_button");
        auto *stop = w.findChild<QToolButton *>("stop_button");
        QCOMPARE(run->size(), stop->size());
        QCOMPARE(run->iconSize(), stop->iconSize());
        QVERIFY(click_channel(resistor));
        const auto screenshot = qEnvironmentVariable("PDS_CHANNEL_SCREENSHOT_PATH");
        if (!screenshot.isEmpty())
            QVERIFY(w.grab().save(screenshot));
        w.set_scope_enabled(false);
        QVERIFY(!item(w, resistor)->data(channel_highlight_role).toBool());
    }
    void independent_label_gestures() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        auto id = w.add_component(Kind::resistor, {0, 0});
        ready(w);
        auto label = [&]() -> QGraphicsItem * {
            for (auto *o : w.canvas()->scene()->items())
                if (o->data(0).toString().toStdString() == id && o->data(1) == "label" &&
                    o->data(2) == "name")
                    return o;
            return nullptr;
        };
        auto *l = label();
        QVERIFY(l);
        const QPointF original = l->pos();
        drag(w, original, original + QPointF(80, 40), Qt::AltModifier);
        QCOMPARE(w.project().components[0].x, 0.);
        QCOMPARE(w.project().components[0].y, 0.);
        QCOMPARE(w.project().labels.size(), size_t(1));
        QVERIFY(QLineF(l->pos(), original + QPointF(80, 40)).length() < 2);
        QTest::keyClick(w.canvas(), Qt::Key_Space);
        QCOMPARE(w.project().labels[0].orientation.quarter_turns, 1u);
        QCOMPARE(w.project().components[0].orientation.quarter_turns, 0u);
        w.undo();
        QCOMPARE(w.project().labels[0].orientation.quarter_turns, 0u);
        w.undo();
        QVERIFY(w.project().labels.empty());
        QCOMPARE(label()->pos(), original);
        auto *vp = w.canvas()->viewport();
        QTest::mousePress(vp, Qt::LeftButton, Qt::AltModifier, w.canvas()->mapFromScene(original));
        QTest::mouseMove(vp, w.canvas()->mapFromScene(original + QPointF(60, 60)), 5);
        QTest::keyClick(w.canvas(), Qt::Key_Space);
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::AltModifier,
                            w.canvas()->mapFromScene(original + QPointF(60, 60)));
        QCOMPARE(w.project().labels.size(), size_t(1));
        QCOMPARE(w.project().labels[0].orientation.quarter_turns, 1u);
        w.undo();
        QCOMPARE(label()->pos(), original);
        QVERIFY(w.project().labels.empty());
        drag(w, original, original + QPointF(40, 40), Qt::AltModifier);
        auto saved = encoded(w.project());
        QVERIFY(w.save_project(dir.filePath("labels.pds")));
        QVERIFY(w.open_project(dir.filePath("labels.pds")));
        QCOMPARE(encoded(w.project()), saved);
        w.select_object(id);
        QTest::keyClick(w.canvas(), Qt::Key_Space);
        QCOMPARE(w.project().components[0].orientation.quarter_turns, 1u);
        QCOMPARE(w.project().labels[0].orientation.quarter_turns, 0u);
    }
    void scope_drag_cursors_and_live_view() {
        init_language("en");
        Result r;
        r.channels = {{"a", "A", "V"}, {"b", "B", "V"}};
        for (int i = 0; i <= 100; ++i)
            r.samples.push_back({i * .1, {i * .2, i * .3}, {}});
        Project p;
        p.scope_end = 10;
        Scope s;
        s.resize(800, 400);
        s.set_result(&r, {0, 1}, p);
        s.show();
        auto *nav = s.navigation();
        s.changed = [&](double a, double b, double ca, double cb) {
            p.scope_begin = a;
            p.scope_end = b;
            p.cursor_a = ca;
            p.cursor_b = cb;
        };
        const double low = s.y_low, high = s.y_high;
        QTest::mousePress(&s, Qt::LeftButton, Qt::NoModifier, {200, 100});
        QTest::mouseMove(&s, {500, 250});
        QTest::mouseRelease(&s, Qt::LeftButton, Qt::NoModifier, {500, 250});
        QVERIFY(s.end - s.begin < 5);
        QCOMPARE(s.y_low, low);
        QCOMPARE(s.y_high, high);
        nav->findChild<QAction *>("view_back")->trigger();
        QCOMPARE(s.begin, 0.);
        QCOMPARE(s.end, 10.);
        nav->findChild<QAction *>("zoom_y")->trigger();
        QTest::mousePress(&s, Qt::LeftButton, Qt::NoModifier, {500, 250});
        QTest::mouseMove(&s, {200, 100});
        QTest::mouseRelease(&s, Qt::LeftButton, Qt::NoModifier, {200, 100});
        QCOMPARE(s.begin, 0.);
        QCOMPARE(s.end, 10.);
        QVERIFY(s.y_high - s.y_low < high - low);
        const double ylow = s.y_low, yhigh = s.y_high;
        QTest::mousePress(&s, Qt::LeftButton, Qt::NoModifier, {100, 80});
        QTest::mouseMove(&s, {600, 300});
        QTest::keyClick(&s, Qt::Key_Escape);
        QTest::mouseRelease(&s, Qt::LeftButton, Qt::NoModifier, {600, 300});
        QCOMPARE(s.y_low, ylow);
        QCOMPARE(s.y_high, yhigh);
        QTest::mousePress(&s, Qt::MiddleButton, Qt::NoModifier, {200, 100});
        QTest::mouseMove(&s, {260, 160});
        QTest::mousePress(&s, Qt::LeftButton, Qt::NoModifier, {260, 160});
        QTest::mouseRelease(&s, Qt::LeftButton, Qt::NoModifier, {260, 160});
        QTest::keyClick(&s, Qt::Key_Escape);
        QTest::mouseRelease(&s, Qt::MiddleButton, Qt::NoModifier, {260, 160});
        QCOMPARE(s.begin, 0.);
        QCOMPARE(s.end, 10.);
        QCOMPARE(s.y_low, ylow);
        QCOMPARE(s.y_high, yhigh);
        s.set_cursor_mode(true);
        s.select_cursor_channel(0, "a");
        s.select_cursor_channel(1, "b");
        s.set_cursor(0, 2);
        s.set_cursor(1, 4);
        QCOMPARE(s.cursor_a, 2.);
        QVERIFY(s.cursor_readout().contains("8 V"));
        QVERIFY(s.cursor_readout().contains("500 mHz"));
        s.set_live(true);
        s.set_result(&r, {0, 1}, p);
        QCOMPARE(s.end, 10.);
        nav->findChild<QAction *>("zoom_xy")->trigger();
        QTest::mousePress(&s, Qt::LeftButton, Qt::NoModifier, {200, 100});
        QTest::mouseRelease(&s, Qt::LeftButton, Qt::NoModifier, {500, 250});
        const double a = s.begin, b = s.end;
        r.samples.push_back({11, {22, 33}, {}});
        s.set_result(&r, {0, 1}, p);
        QCOMPARE(s.begin, a);
        QCOMPARE(s.end, b);
        nav->findChild<QAction *>("follow_live")->trigger();
        QCOMPARE(s.end, 11.);
        s.show_measurements();
        auto *dialog = s.findChild<QDialog *>("scope_measurements");
        QVERIFY(dialog);
        QVERIFY(dialog->findChild<QTableWidget *>("measurement_statistics")->rowCount() > 5);
        s.set_live(false);
        dialog->findChild<QLineEdit *>("trigger_level")->setText("5");
        auto *tabs = dialog->findChild<QTabWidget *>("measurement_tabs");
        tabs->setCurrentIndex(3);
        for (auto *button : dialog->findChildren<QPushButton *>())
            if (button->text() == "Arm")
                button->click();
        QVERIFY(dialog->findChild<QLabel *>("trigger_status")->text().contains("2.5"));
        auto *type = nav->findChild<QComboBox *>("cursor_type");
        type->setCurrentIndex(1);
        s.set_cursor_mode(true);
        QTest::mouseClick(&s, Qt::LeftButton, Qt::NoModifier, {300, 160});
        QVERIFY(s.view_options().free_cursors);
        QVERIFY(std::isfinite(s.view_options().cursor_y_a));
        s.show_display_settings();
        auto *display = s.findChild<QDialog *>("scope_display");
        QVERIFY(display);
        display->findChild<QCheckBox *>("separate_axes")->setChecked(true);
        display->findChild<QSpinBox *>("display_columns")->setValue(2);
        display->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
        QVERIFY(s.view_options().separate_axes);
        QCOMPARE(s.view_options().display_columns, 2u);
        QCOMPARE(s.view_options().signal_displays.size(), size_t(2));
        const double left_low = s.y_low, left_high = s.y_high;
        auto wheel = [&](QPoint point, Qt::KeyboardModifiers modifiers) {
            QWheelEvent event(point, s.mapToGlobal(point), {}, QPoint(0, 120), Qt::NoButton, modifiers,
                              Qt::NoScrollPhase, false);
            QApplication::sendEvent(&s, &event);
        };
        wheel({600, 200}, Qt::ShiftModifier);
        wheel({200, 200}, Qt::ControlModifier);
        QCOMPARE(s.y_low, left_low);
        QCOMPARE(s.y_high, left_high);
        QVERIFY(!s.grab().isNull());
        Scope restored;
        restored.load_view_options(s.view_options());
        restored.set_result(&r, {0, 1}, p);
        QCOMPARE(restored.view_options().cursor_channel_b, std::string("b"));
        QVERIFY(restored.view_options().separate_axes);
        QCOMPARE(restored.view_options().display_columns, 2u);
        auto screenshot = qEnvironmentVariable("PDS_SCOPE_GRID_SCREENSHOT_PATH");
        if (!screenshot.isEmpty())
            QVERIFY(s.grab().save(screenshot));
        delete nav;
    }
    void repeated_and_automatic_scope_trigger() {
        init_language("en");
        Result r;
        r.channels = {{"a", "A", "V"}};
        r.samples = {{0, {0}, {}}, {1, {1}, {}}, {2, {0}, {}}};
        Project p;
        p.scope_end = 2;
        Scope s;
        s.resize(800, 400);
        s.set_result(&r, {0}, p);
        s.show();
        s.set_live(true);
        s.changed = [&](double a, double b, double ca, double cb) {
            p.scope_begin = a;
            p.scope_end = b;
            p.cursor_a = ca;
            p.cursor_b = cb;
        };
        s.show_measurements();
        auto *d = s.findChild<QDialog *>("scope_measurements");
        QVERIFY(d);
        d->findChild<QTabWidget *>("measurement_tabs")->setCurrentIndex(3);
        d->findChild<QLineEdit *>("trigger_level")->setText("0.5");
        d->findChild<QLineEdit *>("trigger_holdoff")->setText("3");
        d->findChild<QComboBox *>("trigger_mode")->setCurrentIndex(1);
        auto arm = [&] {
            for (auto *button : d->findChildren<QPushButton *>())
                if (button->text() == "Arm")
                    button->click();
        };
        arm();
        r.samples.push_back({3, {1}, {}});
        s.set_result(&r, {0}, p);
        QVERIFY(std::abs(s.begin - 2.1) < 1e-10);
        double first = s.begin;
        r.samples.push_back({4, {0}, {}});
        r.samples.push_back({5, {1}, {}});
        s.set_result(&r, {0}, p);
        QCOMPARE(s.begin, first);
        r.samples.push_back({6, {0}, {}});
        r.samples.push_back({7, {1}, {}});
        s.set_result(&r, {0}, p);
        QVERIFY(std::abs(s.begin - 6.1) < 1e-10);
        d->findChild<QComboBox *>("trigger_mode")->setCurrentIndex(2);
        arm();
        for (int i = 8; i <= 12; ++i)
            r.samples.push_back({double(i), {0}, {}});
        s.set_result(&r, {0}, p);
        QCOMPARE(s.end, 12.);
        d->findChild<QComboBox *>("trigger_mode")->setCurrentIndex(0);
        d->findChild<QComboBox *>("trigger_edge")->setCurrentIndex(2);
        arm();
        r.samples.push_back({13, {1}, {}});
        s.set_result(&r, {0}, p);
        first = s.begin;
        r.samples.push_back({14, {0}, {}});
        r.samples.push_back({15, {1}, {}});
        s.set_result(&r, {0}, p);
        QCOMPARE(s.begin, first);
    }
    void configurable_scope_wheel_modifiers() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        ready(w);
        bool saved = false;
        QTimer::singleShot(20, &w, [&] {
            auto *d = w.findChild<QDialog *>("shortcuts_dialog");
            if (!d)
                return;
            auto *x = d->findChild<QComboBox *>("scope_wheel_x"),
                 *y = d->findChild<QComboBox *>("scope_wheel_y");
            x->setCurrentIndex(x->findData(int(Qt::AltModifier)));
            y->setCurrentIndex(y->findData(int(Qt::AltModifier)));
            d->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
            QVERIFY(d->isVisible());
            QVERIFY(!d->findChild<QLabel *>("shortcut_error")->text().isEmpty());
            y->setCurrentIndex(y->findData(int(Qt::ControlModifier | Qt::ShiftModifier)));
            d->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
            saved = true;
        });
        w.show_shortcuts();
        QVERIFY(saved);
        QSettings settings(dir.filePath("shortcuts.ini"), QSettings::IniFormat);
        QCOMPARE(settings.value("scope/wheel_x").toInt(), int(Qt::AltModifier));
        EditorWindow next("en", dir.path());
        bool loaded = false;
        QTimer::singleShot(20, &next, [&] {
            auto *d = next.findChild<QDialog *>("shortcuts_dialog");
            if (!d)
                return;
            QCOMPARE(d->findChild<QComboBox *>("scope_wheel_x")->currentData().toInt(), int(Qt::AltModifier));
            loaded = true;
            d->reject();
        });
        next.show_shortcuts();
        QVERIFY(loaded);
    }
    void live_simulation_pause_and_stop() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        ready(w);
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        w.set_scope_enabled(true);
        auto *list = w.findChild<QListWidget *>("channels");
        QVERIFY(list && list->count());
        list->item(0)->setCheckState(Qt::Checked);
        w.findChild<QLineEdit *>("sim_stop")->setText("1");
        w.findChild<QLineEdit *>("sim_step")->setText("1e-7");
        w.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(w.has_result() && w.result().samples.size() > 100, 5000);
        QVERIFY(w.running());
        QVERIFY(list->isEnabled());
        const auto recorded = w.project().scope_channels;
        list->item(0)->setCheckState(Qt::Unchecked);
        QCOMPARE(list->item(0)->checkState(), Qt::Checked);
        QCOMPARE(w.project().scope_channels, recorded);
        list->setCurrentRow(0);
        auto *run_button = w.findChild<QToolButton *>("run_button");
        auto *stop_button = w.findChild<QToolButton *>("stop_button");
        QCOMPARE(run_button->size(), stop_button->size());
        w.start_simulation();
        QCOMPARE(w.findChild<QAction *>("action_run")->text(), QString("Resume"));
        QCOMPARE(run_button->size(), stop_button->size());
        QTest::qWait(150);
        const auto size = w.result().samples.size();
        QTest::qWait(150);
        QCOMPARE(w.result().samples.size(), size);
        w.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(w.result().samples.size() > size, 2000);
        w.start_simulation();
        w.stop_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 2000);
        QVERIFY(w.result().cancelled);
        for (size_t i = 1; i < w.result().samples.size(); ++i)
            QVERIFY(w.result().samples[i].time > w.result().samples[i - 1].time);
    }
    void inline_labels_and_configured_properties() {
        QTemporaryDir dir, configs;
        QFile original(":/components/resistor.json");
        // Resource initialization is performed by EditorWindow.
        init_language("en");
        QVERIFY(original.open(QIODevice::ReadOnly));
        auto spec = QJsonDocument::fromJson(original.readAll()).object();
        auto fields = spec.value("fields").toArray();
        fields.append(QJsonObject{{"key", "x"},
                                  {"editor", "number"},
                                  {"label", "Horizontal position"},
                                  {"min", -500},
                                  {"max", 500}});
        spec["fields"] = fields;
        QFile overrideFile(configs.filePath("resistor.json"));
        QVERIFY(overrideFile.open(QIODevice::WriteOnly));
        overrideFile.write(QJsonDocument(spec).toJson());
        overrideFile.close();
        auto previous = qgetenv("PDS_COMPONENT_DIR");
        qputenv("PDS_COMPONENT_DIR", configs.path().toUtf8());
        EditorWindow w("en", dir.path());
        if (previous.isNull())
            qunsetenv("PDS_COMPONENT_DIR");
        else
            qputenv("PDS_COMPONENT_DIR", previous);
        auto id = w.add_component(Kind::resistor, {0, 0});
        ready(w);
        w.select_object(id);
        auto *x = w.findChild<QLineEdit *>("property_x");
        QVERIFY(x && x->isVisible());
        x->setText("40");
        QTest::mouseClick(w.findChild<QPushButton *>("apply_properties"), Qt::LeftButton);
        QCOMPARE(w.project().components.front().x, 40.);
        w.undo();
        QCOMPARE(w.project().components.front().x, 0.);
        auto edit_at = [&](QPointF point) {
            QTest::mouseDClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                               w.canvas()->mapFromScene(point));
            return w.canvas()->viewport()->findChild<QLineEdit *>("inline_property");
        };
        auto *edit = edit_at({0, -40});
        QVERIFY(edit && edit->isVisible());
        edit->setText("2 kOhm");
        QTest::keyClick(edit, Qt::Key_Return);
        QCOMPARE(w.project().components.front().value, 2000.);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        w.undo();
        QCOMPARE(w.project().components.front().value, 1000.);
        edit = edit_at({0, -40});
        QVERIFY(edit && edit->isVisible());
        edit->setText("wrong");
        QTest::keyClick(edit, Qt::Key_Return);
        QCOMPARE(w.project().components.front().value, 1000.);
        QVERIFY(edit->isVisible());
        QTest::keyClick(edit, Qt::Key_Escape);
        QVERIFY(!edit->isVisible());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        edit = edit_at({0, 40});
        QVERIFY(edit && edit->isVisible());
        edit->setText("Load");
        QTest::keyClick(edit, Qt::Key_Return);
        QCOMPARE(w.project().components.front().name, std::string("Load"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        w.findChild<QAction *>("insert_component_104")->trigger();
        QTest::mouseClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                          w.canvas()->mapFromScene(QPointF(320, 0)));
        edit = edit_at({350, -40});
        QVERIFY(edit && edit->isVisible());
        QCOMPARE(edit->text(), QString("50"));
        edit->setText("25");
        QTest::keyClick(edit, Qt::Key_Return);
        QCOMPARE(w.project().patterns.front().duty, .25);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        w.select_object(id);
        w.canvas()->setFocus();
        w.findChild<QAction *>("action_rotate")->trigger();
        auto *rotated = item(w, id);
        edit = edit_at(rotated->mapToScene(QPointF(0, -41)));
        QVERIFY(edit && edit->isVisible());
        edit->setText("3 kOhm");
        QVERIFY(w.save_project(dir.filePath("inline.pds")));
        QCOMPARE(w.project().components.front().value, 3000.);
        QVERIFY(w.open_project(dir.filePath("inline.pds")));
        QCOMPARE(w.project().components.front().value, 3000.);
    }
    void scope_zoom_and_fit_axes() {
        init_language("en");
        Result result;
        result.channels.push_back({"signal", "signal", "V"});
        for (int i = 0; i <= 100; ++i)
            result.samples.push_back({i * .1, {std::sin(i * .1)}, {}});
        Project p;
        p.scope_begin = 0;
        p.scope_end = 10;
        Scope scope;
        scope.resize(800, 400);
        scope.set_result(&result, {0}, p);
        scope.show();
        auto *navigation = scope.navigation();
        auto wheel = [&](Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            QPoint point(400, 200);
            QWheelEvent event(point, scope.mapToGlobal(point), {}, QPoint(0, 120), Qt::NoButton, modifiers,
                              Qt::NoScrollPhase, false);
            QApplication::sendEvent(&scope, &event);
        };
        double low = scope.y_low, high = scope.y_high;
        navigation->findChild<QAction *>("zoom_x")->trigger();
        wheel(Qt::ControlModifier);
        QVERIFY(scope.end - scope.begin < 10);
        QCOMPARE(scope.y_low, low);
        QCOMPARE(scope.y_high, high);
        const double begin = scope.begin, end = scope.end;
        navigation->findChild<QAction *>("zoom_y")->trigger();
        wheel(Qt::ShiftModifier);
        QCOMPARE(scope.begin, begin);
        QCOMPARE(scope.end, end);
        QVERIFY(scope.y_high - scope.y_low < high - low);
        low = scope.y_low;
        high = scope.y_high;
        navigation->findChild<QAction *>("fit_x")->trigger();
        QCOMPARE(scope.begin, 0.);
        QCOMPARE(scope.end, 10.);
        QCOMPARE(scope.y_low, low);
        QCOMPARE(scope.y_high, high);
        navigation->findChild<QAction *>("zoom_xy")->trigger();
        wheel();
        QVERIFY(scope.end - scope.begin < 10);
        QVERIFY(scope.y_high - scope.y_low < high - low);
        const double zoom_begin = scope.begin, zoom_end = scope.end;
        navigation->findChild<QAction *>("fit_y")->trigger();
        QCOMPARE(scope.begin, zoom_begin);
        QCOMPARE(scope.end, zoom_end);
        scope.fit();
        QCOMPARE(scope.begin, 0.);
        QCOMPARE(scope.end, 10.);
        Scope other;
        other.set_result(&result, {0}, p);
        QCOMPARE(other.begin, 0.);
        QCOMPARE(other.end, 10.);
        Result dense;
        dense.channels = result.channels;
        for (int i = 0; i <= 10000; ++i)
            dense.samples.push_back({i * .001, {i == 1234 ? 10. : i == 6789 ? -7. : 0.}, {}});
        scope.set_result(&dense, {0}, p);
        scope.fit();
        const auto rendered = scope.grab().toImage().scaled(scope.size());
        bool positive = false, negative = false;
        for (int y = 13; y < 351; ++y)
            for (int x = 59; x < 774; ++x) {
                const auto color = rendered.pixelColor(x, y);
                if (color.blue() > 150 && color.red() < 50 && color.green() > 60 && color.green() < 160) {
                    positive |= y < 100;
                    negative |= y > 300;
                }
            }
        QVERIFY(positive && negative); // Single-sample extrema survive dense rendering.
        delete navigation;
    }
    void delete_pass_through_node_preserves_wire() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        auto a = w.add_component(Kind::resistor, {-180, 0});
        auto b = w.add_component(Kind::resistor, {180, 0});
        auto node = w.add_node(false, {0, 0});
        QVERIFY(w.connect_ports({a, "n"}, {node, "node"}));
        QVERIFY(w.connect_ports({node, "node"}, {b, "p"}));
        ready(w);
        const auto before = encoded(w.project());
        w.select_object(node);
        QTest::keyClick(w.canvas(), Qt::Key_Delete);
        QVERIFY(w.project().nodes.empty());
        QCOMPARE(w.project().components.size(), size_t(2));
        QCOMPARE(w.project().wires.size(), size_t(1));
        const auto &wire = w.project().wires.front();
        QVERIFY(wire.from.object == a || wire.to.object == a);
        QVERIFY(wire.from.object == b || wire.to.object == b);
        w.undo();
        QCOMPARE(encoded(w.project()), before);
    }
    void component_toolbar_and_pins_persist() {
        QTemporaryDir dir;
        {
            EditorWindow w("en", dir.path());
            ready(w);
            auto *tree = w.findChild<QTreeWidget *>("library");
            auto *bar = w.findChild<QToolBar *>("component_bar");
            QVERIFY(tree && bar);
            QCOMPARE(tree->topLevelItemCount(), 4);
            QCOMPARE(bar->actions().size(), 4);
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                QVERIFY(!tree->topLevelItem(i)->isExpanded());
                for (int j = 0; j < tree->topLevelItem(i)->childCount(); ++j) {
                    int id = tree->topLevelItem(i)->child(j)->data(0, Qt::UserRole).toInt();
                    QVERIFY(id != 100 && id != 7 && id != 8 && id != 103);
                }
            }
            auto *search = w.findChild<QLineEdit *>("library_search");
            search->setText("Resistor");
            QVERIFY(tree->topLevelItem(0)->isExpanded());
            search->clear();
            QVERIFY(!tree->topLevelItem(0)->isExpanded());
            for (int id : {100, 7, 8, 103}) {
                auto before = encoded(w.project());
                auto *action = w.findChild<QAction *>("insert_component_" + QString::number(id));
                QVERIFY(bar->widgetForAction(action)->isVisible());
                QTest::mouseClick(bar->widgetForAction(action), Qt::LeftButton);
                QCOMPARE(encoded(w.project()), before);
                QTest::mouseClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                                  w.canvas()->mapFromScene(QPointF(120, 80)));
                if (id == 100)
                    QVERIFY(w.project().nodes.back().ground);
                else if (id == 103)
                    QCOMPARE(w.project().plots.size(), size_t(1));
                else
                    QCOMPARE(static_cast<int>(w.project().components.back().kind), id);
                w.undo();
                QCOMPARE(encoded(w.project()), before);
            }
            auto before = encoded(w.project());
            auto *category = tree->topLevelItem(0);
            category->setExpanded(true);
            auto *resistor = category->child(0);
            auto at = tree->visualItemRect(resistor).center();
            QTimer::singleShot(0, &w, [&] {
                auto *menu = w.findChild<QMenu *>("component_pin_menu");
                QVERIFY(menu);
                menu->actions().front()->trigger();
                menu->close();
            });
            QContextMenuEvent event(QContextMenuEvent::Mouse, at, tree->viewport()->mapToGlobal(at));
            QApplication::sendEvent(tree->viewport(), &event);
            auto *r = w.findChild<QAction *>("insert_component_0");
            QVERIFY(bar->actions().contains(r));
            QTRY_VERIFY(bar->widgetForAction(r)->isVisible());
            QCOMPARE(encoded(w.project()), before);
            QTest::mouseClick(bar->widgetForAction(r), Qt::LeftButton);
            QTest::mouseClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                              w.canvas()->mapFromScene(QPointF(120, 80)));
            QCOMPARE(w.project().components.back().kind, Kind::resistor);
            if (qEnvironmentVariableIsSet("PDS_PALETTE_SCREENSHOT")) {
                QTest::qWait(30);
                QVERIFY(w.grab().save(qEnvironmentVariable("PDS_PALETTE_SCREENSHOT")));
            }
        }
        {
            EditorWindow w("en", dir.path());
            ready(w);
            auto *bar = w.findChild<QToolBar *>("component_bar");
            auto *r = w.findChild<QAction *>("insert_component_0");
            QVERIFY(bar->actions().contains(r));
            auto at = bar->actionGeometry(r).center();
            QTimer::singleShot(0, &w, [&] {
                auto *menu = w.findChild<QMenu *>("component_unpin_menu");
                QVERIFY(menu);
                menu->actions().front()->trigger();
                menu->close();
            });
            QContextMenuEvent event(QContextMenuEvent::Mouse, at, bar->mapToGlobal(at));
            QApplication::sendEvent(bar, &event);
            QVERIFY(!bar->actions().contains(r));
        }
        EditorWindow w("en", dir.path());
        QCOMPARE(w.findChild<QToolBar *>("component_bar")->actions().size(), 4);
    }
    void wire_second_click_selects_straight_segment_without_frame() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        auto a = w.add_node(false, {0, 0}), b = w.add_node(false, {240, 0});
        QVERIFY(w.connect_ports({a, "node"}, {b, "node"}));
        auto p = w.project();
        p.wires[0].bends = {{0, 120}, {120, 120}, {240, 120}};
        w.set_project(p);
        ready(w);
        auto *c = w.canvas();
        auto *vp = c->viewport();
        auto *wire = static_cast<QGraphicsPathItem *>(item(w, p.wires[0].id));
        QCOMPARE(wire->path().elementCount(), 4); // Collinear stored points are one visible segment.
        const auto at = c->mapFromScene(QPointF(120, 120));
        QTest::mouseClick(vp, Qt::LeftButton, Qt::NoModifier, at);
        QVERIFY(wire->isSelected());
        QCOMPARE(wire->data(wire_segment_role).toInt(), 0);
        QCOMPARE(wire->pen().color(), QColor("#e88b22"));
        auto render = [&](bool selected) {
            QImage image(280, 160, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.translate(20, 20);
            QStyleOptionGraphicsItem option;
            if (selected)
                option.state |= QStyle::State_Selected;
            wire->paint(&painter, &option, nullptr);
            return image;
        };
        QCOMPARE(render(true), render(false)); // Qt's selection rectangle must not be painted.
        QTest::mouseClick(vp, Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(wire->data(wire_segment_role).toInt(), 2);
        QCOMPARE(wire->pen().color(), QColor("#146cca"));
        const auto preview = render(true);
        QCOMPARE(preview.pixelColor(140, 140), QColor("#e88b22"));
        QCOMPARE(preview.pixelColor(20, 80), QColor("#146cca"));
        drag(w, {120, 120}, {120, 160});
        QCOMPARE(wire->path().elementAt(1).y, 160.0);
        QCOMPARE(wire->path().elementAt(2).y, 160.0);
        w.undo();
        QCOMPARE(wire->path().elementAt(1).y, 120.0);
        QTest::mouseClick(vp, Qt::LeftButton, Qt::NoModifier, c->mapFromScene(QPointF(350, 180)));
        QVERIFY(!wire->isSelected());
        QCOMPARE(wire->data(wire_segment_role).toInt(), 0);
        QTest::mouseClick(vp, Qt::LeftButton, Qt::NoModifier, at);
        QTest::mouseDClick(vp, Qt::LeftButton, Qt::NoModifier, at);
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(wire->data(wire_segment_role).toInt(), 2);
        if (qEnvironmentVariableIsSet("PDS_WIRE_SCREENSHOT"))
            QVERIFY(vp->grab().save(qEnvironmentVariable("PDS_WIRE_SCREENSHOT")));
    }
    void move_rotate_mirror_pan_and_cancel() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        auto id = w.add_component(Kind::resistor, {0, 0});
        ready(w);
        auto *c = w.canvas();
        auto *vp = c->viewport();
        auto *block = item(w, id);
        const auto original = encoded(w.project());
        auto start = c->mapFromScene(QPointF(0, 0));
        auto at = c->mapFromScene(QPointF(80, 40));
        QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(vp, at, 5);
        QCOMPARE(block->pos(), QPointF(80, 40));
        QTest::keyClick(c, Qt::Key_Space);
        QCOMPARE(block->pos(), QPointF(80, 40));
        QCOMPARE(block->transform().map(QPointF(60, 0)), QPointF(0, 60));
        QCOMPARE(encoded(w.project()), original);
        QTest::keyClick(c, Qt::Key_M, Qt::ControlModifier);
        QCOMPARE(block->pos(), QPointF(80, 40));
        QVERIFY(block->transform().determinant() < 0);
        QTest::mousePress(vp, Qt::MiddleButton, Qt::NoModifier, at);
        QTest::mouseMove(vp, at + QPoint(40, 20), 5);
        QTest::mouseRelease(vp, Qt::MiddleButton, Qt::NoModifier, at + QPoint(40, 20));
        QCOMPARE(block->pos(), QPointF(80, 40));
        auto end = c->mapFromScene(QPointF(140, 80));
        QTest::mouseMove(vp, end, 5);
        QCOMPARE(block->pos(), QPointF(140, 80));
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, end);
        QCOMPARE(w.project().components[0].x, 140.0);
        QCOMPARE(w.project().components[0].orientation.quarter_turns, 1u);
        QVERIFY(w.project().components[0].orientation.mirrored);
        auto final = encoded(w.project());
        w.undo();
        QCOMPARE(encoded(w.project()), original);
        w.redo();
        QCOMPARE(encoded(w.project()), final);
        QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, end);
        QTest::mouseMove(vp, end + QPoint(40, 40), 5);
        QTest::keyClick(c, Qt::Key_Space);
        QTest::keyClick(c, Qt::Key_Escape);
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, end + QPoint(40, 40));
        QCOMPARE(encoded(w.project()), final);
        QCOMPARE(block->pos(), QPointF(140, 80));
        QCOMPARE(block->transform().map(QPointF(60, 0)), QPointF(0, 60));
        QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, end);
        const auto zoomAt = end + QPoint(40, 40);
        QTest::mouseMove(vp, zoomAt, 5);
        QWheelEvent wheel(QPointF(zoomAt), vp->mapToGlobal(zoomAt), {}, QPoint(0, 120), Qt::LeftButton,
                          Qt::AltModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(vp, &wheel);
        QVERIFY(QLineF(QPointF(c->mapFromScene(block->pos())), QPointF(zoomAt)).length() < 2);
        QTest::keyClick(c, Qt::Key_Escape);
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, zoomAt);
        QCOMPARE(encoded(w.project()), final);
    }
    void pwm_replaces_example_events_atomically() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/diode-freewheel.pds"));
        auto sw = std::find_if(w.project().components.begin(), w.project().components.end(),
                               [](const Component &c) { return c.kind == Kind::ideal_switch; })
                      ->id;
        auto pattern = w.add_pattern({160, -140});
        auto project = w.project();
        project.patterns.back().pwm = true;
        project.patterns.back().frequency = 1000;
        project.patterns.back().duty = .5;
        w.set_project(project);
        ready(w);
        w.canvas()->centerOn(160, 0);
        QVERIFY(!w.project().events.empty());
        const auto original = encoded(w.project());
        QTimer::singleShot(0, &w, [&] {
            auto *box = w.findChild<QMessageBox *>("gate_replace_dialog");
            QVERIFY(box);
            box->reject();
        });
        drag(w, w.port_position({pattern, "out"}), w.port_position({sw, "gate"}));
        QCOMPARE(encoded(w.project()), original);
        QTimer::singleShot(0, &w, [&] {
            auto *box = w.findChild<QMessageBox *>("gate_replace_dialog");
            QVERIFY(box);
            for (auto *button : box->buttons())
                if (box->buttonRole(button) == QMessageBox::AcceptRole)
                    button->click();
        });
        drag(w, w.port_position({sw, "gate"}), w.port_position({pattern, "out"}));
        QVERIFY(w.project().events.empty());
        QVERIFY(std::any_of(w.project().wires.begin(), w.project().wires.end(), [&](const Wire &wire) {
            return wire.from.object == pattern || wire.to.object == pattern;
        }));
        resolve_connections(w.project());
        const auto final = encoded(w.project());
        w.undo();
        QCOMPARE(encoded(w.project()), original);
        w.redo();
        QCOMPARE(encoded(w.project()), final);
        w.select_object(sw);
        QVERIFY(!w.findChild<QTableWidget *>("property_events")->isVisible());
        w.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 10000);
        QVERIFY(w.has_result());
    }
    void group_transform_preserves_routes_and_undo_cancels_only_gesture() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        auto a = w.add_component(Kind::resistor, {0, 0});
        auto b = w.add_component(Kind::resistor, {240, 0});
        QVERIFY(w.connect_ports({a, "n"}, {b, "p"}));
        auto p = w.project();
        p.wires[0].bends = {{120, 40}, {160, 40}};
        w.set_project(p);
        ready(w);
        w.canvas()->centerOn(120, 140);
        auto *c = w.canvas();
        auto *vp = c->viewport();
        item(w, a)->setSelected(true);
        item(w, b)->setSelected(true);
        auto before = encoded(w.project());
        QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, c->mapFromScene(QPointF(0, 0)));
        QTest::mouseMove(vp, c->mapFromScene(QPointF(40, 40)), 5);
        QTest::keyClick(c, Qt::Key_Space);
        QCOMPARE(item(w, a)->pos(), QPointF(40, 40));
        QCOMPARE(item(w, b)->pos(), QPointF(40, 280));
        QTest::mouseMove(vp, c->mapFromScene(QPointF(80, 40)), 5);
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, c->mapFromScene(QPointF(80, 40)));
        QCOMPARE(w.project().wires[0].bends[0].x, 40.0);
        QCOMPARE(w.project().wires[0].bends[0].y, 160.0);
        auto final = encoded(w.project());
        QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, c->mapFromScene(QPointF(80, 40)));
        QTest::mouseMove(vp, c->mapFromScene(QPointF(120, 80)), 5);
        QTest::keyClick(c, Qt::Key_Space);
        QTest::keyClick(c, Qt::Key_Z, Qt::ControlModifier);
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, c->mapFromScene(QPointF(120, 80)));
        QCOMPARE(encoded(w.project()), final);
        w.undo();
        QCOMPARE(encoded(w.project()), before);
        w.redo();
        QCOMPARE(encoded(w.project()), final);
    }
    void group_move_cancel_and_results() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        ready(w);
        w.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 10000);
        QVERIFY(w.has_result());
        auto a = w.project().components[0].id, b = w.project().components[1].id;
        auto *first = item(w, a);
        auto *second = item(w, b);
        QVERIFY(first && second);
        w.canvas()->scene()->clearSelection();
        first->setSelected(true);
        second->setSelected(true);
        auto pa = first->pos(), pb = second->pos();
        w.canvas()->centerOn(pa + QPointF(100, 40));
        drag(w, pa, pa + QPointF(40, 40));
        QCOMPARE(item(w, a), first);
        QCOMPARE(item(w, b), second);
        QCOMPARE(first->pos(), pa + QPointF(40, 40));
        QCOMPARE(second->pos(), pb + QPointF(40, 40));
        QVERIFY(w.has_result());
        QVERIFY(first->isSelected() && second->isSelected());
        auto before = encoded(w.project());
        auto origin = first->pos();
        auto *viewport = w.canvas()->viewport();
        QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, w.canvas()->mapFromScene(origin));
        QTest::mouseMove(viewport, w.canvas()->mapFromScene(origin + QPointF(80, 40)), 5);
        QTest::keyClick(w.canvas(), Qt::Key_Escape);
        QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier,
                            w.canvas()->mapFromScene(origin + QPointF(80, 40)));
        QCOMPARE(encoded(w.project()), before);
        QCOMPARE(first->pos(), origin);
        w.undo();
        QCOMPARE(first->pos(), pa);
        QCOMPARE(second->pos(), pb);
        QVERIFY(w.has_result());
        w.redo();
        QCOMPARE(first->pos(), origin);
    }
    void paste_preview_rotate_and_cancel() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        auto r = w.add_component(Kind::resistor, {0, 0});
        ready(w);
        w.select_object(r);
        QTest::keyClick(w.canvas(), Qt::Key_C, Qt::ControlModifier);
        QTest::keyClick(w.canvas(), Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(w.project().components.size(), size_t(1));
        QTest::keyClick(w.canvas(), Qt::Key_Space);
        QCOMPARE(w.project().components[0].orientation.quarter_turns, 0u);
        QTest::mouseClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                          w.canvas()->mapFromScene(QPointF(240, 100)));
        QCOMPARE(w.project().components.size(), size_t(2));
        QCOMPARE(w.project().components[1].orientation.quarter_turns, 1u);
        auto before = encoded(w.project());
        QTest::keyClick(w.canvas(), Qt::Key_V, Qt::ControlModifier);
        QTest::keyClick(w.canvas(), Qt::Key_Escape);
        QCOMPARE(encoded(w.project()), before);
        w.undo();
        QCOMPARE(w.project().components.size(), size_t(1));
        w.redo();
        QCOMPARE(w.project().components.size(), size_t(2));
        // Closing an editor with an active preview must release its callbacks before member teardown.
        QTest::keyClick(w.canvas(), Qt::Key_V, Qt::ControlModifier);
    }
    void moving_component_reuses_connected_trunk() {
        // A branch reuses the existing vertical run of its own network.
        auto trunk = manual_route({20, 200}, {0, 0}, {{0, 200}});
        auto branch = orthogonal_route({140, 160}, {120, 160}, {0, 0}, {0, 0}, {});
        const auto joined = join_route_to_trunk(branch, {120, 160}, trunk, {});
        QVERIFY(joined);
        QCOMPARE(joined->second, QPointF(0, 160));
        QCOMPARE(joined->first.elementCount(), 3);
        // Crossing another network is not permission to join it.
        QVERIFY(!join_route_to_trunk(branch, {120, 160}, manual_route({20, 200}, {0, -20}, {{0, 200}}), {}));
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        auto common = w.add_node(false, {0, -100});
        auto diode = w.add_component(Kind::diode, {-40, 180});
        auto resistor = w.add_component(Kind::resistor, {160, 120});
        auto intermediate = w.add_node(false, {0, -60});
        QVERIFY(w.connect_ports({diode, "n"}, {intermediate, "node"}));
        const auto trunk_id = w.project().wires.back().id;
        QVERIFY(w.connect_ports({intermediate, "node"}, {common, "node"}));
        QVERIFY(w.connect_ports({resistor, "p"}, {common, "node"}));
        const auto branch_id = w.project().wires.back().id;
        ready(w);
        const auto before = encoded(w.project());
        drag(w, {160, 120}, {200, 80});
        auto route = static_cast<QGraphicsPathItem *>(item(w, branch_id))->path();
        auto trunk_path = static_cast<QGraphicsPathItem *>(item(w, trunk_id))->path();
        const auto port = w.port_position({resistor, "p"});
        // The branch turns onto the trunk at its own height, not at the old node.
        QVERIFY(route.elementCount() >= 3);
        const QPointF turn(route.elementAt(1).x, route.elementAt(1).y);
        QCOMPARE(turn.y(), port.y());
        QCOMPARE(project_on_route(trunk_path, turn), turn);
        const auto moved = encoded(w.project());
        w.undo();
        QCOMPARE(encoded(w.project()), before);
        w.redo();
        QCOMPARE(encoded(w.project()), moved);
        QCOMPARE(static_cast<QGraphicsPathItem *>(item(w, branch_id))->path(), route);
        QVERIFY(w.save_project(dir.filePath("shared.pds")));
        QVERIFY(w.open_project(dir.filePath("shared.pds")));
        QCOMPARE(static_cast<QGraphicsPathItem *>(item(w, branch_id))->path(), route);
        const auto screenshot = qEnvironmentVariable("PDS_SHARED_WIRE_SCREENSHOT_PATH");
        if (!screenshot.isEmpty()) {
            const auto project_path = qEnvironmentVariable("PDS_SHARED_WIRE_PROJECT");
            if (!project_path.isEmpty())
                QVERIFY(w.open_project(project_path));
            w.canvas()->resetTransform();
            w.canvas()->centerOn(260, 80);
            QVERIFY(w.grab().save(screenshot));
        }
    }
    void moved_plot_routes_have_no_backtracking() {
        // Fractional bends reproduce the small spur on a saved graph wire.
        const std::vector<Point> bends{{150, 89}, {150, 84.6}, {0, 84.6}};
        const QPointF node(0, 80.2);
        const auto up = manual_route({220, -71}, node, bends);
        QCOMPARE(up.elementCount(), 5);
        QCOMPARE(QPointF(up.elementAt(1).x, up.elementAt(1).y), QPointF(150, -71));
        QCOMPARE(QPointF(up.elementAt(2).x, up.elementAt(2).y), QPointF(150, 84.6));
        auto no_spurs = [](const QPainterPath &path) {
            for (int i = 1; i + 1 < path.elementCount(); ++i) {
                auto a = path.elementAt(i - 1), b = path.elementAt(i), c = path.elementAt(i + 1);
                QPointF u(b.x - a.x, b.y - a.y), v(c.x - b.x, c.y - b.y);
                if (std::abs(u.x() * v.y() - u.y() * v.x()) < 1e-8 && QPointF::dotProduct(u, v) < 0)
                    return false;
            }
            return true;
        };
        QVERIFY(no_spurs(up));
        for (double y : {-160., 80.2, 84.6, 89., 180.})
            QVERIFY(no_spurs(manual_route({220, y}, node, bends)));
        // A complete out-and-back run collapses without leaving duplicate vertices.
        QCOMPARE(manual_route({0, 0}, {100, 0}, {{40, 0}, {0, 0}}).elementCount(), 2);
        // Real orthogonal detours remain intact.
        const auto detour = manual_route({0, 0}, {200, 0}, {{40, 0}, {40, 80}, {160, 80}, {160, 0}});
        QCOMPARE(detour.elementCount(), 6);
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        auto source = w.add_node(false, {0, 80});
        auto plot = w.add_plot({290, 100});
        QVERIFY(w.connect_ports({plot, "in1"}, {source, "node"}));
        Project p = w.project();
        p.wires.front().bends = bends;
        const auto wire = p.wires.front().id;
        QFile file(dir.filePath("route.pds"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray::fromStdString(encoded(p)));
        file.close();
        QVERIFY(w.open_project(file.fileName()));
        ready(w);
        const auto before = encoded(w.project());
        auto *canvas = w.canvas();
        QTest::mousePress(canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                          canvas->mapFromScene({290, 100}));
        QTest::mouseMove(canvas->viewport(), canvas->mapFromScene({290, -100}), 5);
        auto preview = static_cast<QGraphicsPathItem *>(item(w, wire))->path();
        QVERIFY(no_spurs(preview));
        QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, Qt::NoModifier,
                            canvas->mapFromScene({290, -100}));
        QCOMPARE(static_cast<QGraphicsPathItem *>(item(w, wire))->path(), preview);
        QVERIFY(encoded(w.project()) != before);
        const auto after = encoded(w.project());
        w.undo();
        QCOMPARE(encoded(w.project()), before);
        w.redo();
        QCOMPARE(encoded(w.project()), after);
        QVERIFY(w.save_project(file.fileName()));
        QVERIFY(w.open_project(file.fileName()));
        QCOMPARE(static_cast<QGraphicsPathItem *>(item(w, wire))->path(), preview);
        const auto screenshot = qEnvironmentVariable("PDS_ROUTE_SCREENSHOT_PATH");
        if (!screenshot.isEmpty()) {
            w.canvas()->centerOn(150, 0);
            QVERIFY(w.grab().save(screenshot));
        }
    }
    void wires_branch_route_reconnect_and_save() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        auto a = w.add_node(false, {0, 0}), b = w.add_node(false, {240, 0}),
             c = w.add_node(false, {120, 180}), d = w.add_node(false, {300, 180});
        QVERIFY(w.connect_ports({a, "node"}, {b, "node"}));
        ready(w);
        auto wire = w.project().wires.front().id;
        w.canvas()->scene()->clearSelection();
        auto before = encoded(w.project());
        drag(w, {120, 0}, {120, 180}, Qt::ControlModifier);
        QCOMPARE(w.project().wires.size(), size_t(3));
        QCOMPARE(w.project().nodes.size(), size_t(5));
        w.undo();
        QCOMPARE(encoded(w.project()), before);
        w.redo();
        w.select_object(wire);
        auto *path = static_cast<QGraphicsPathItem *>(item(w, wire));
        auto mid = path->path().pointAtPercent(.5);
        drag(w, mid, mid + QPointF(0, 40));
        QVERIFY(!w.project().wires.front().bends.empty());
        auto route = encoded(w.project());
        w.undo();
        w.redo();
        QCOMPARE(encoded(w.project()), route);
        w.select_object(wire);
        auto end = path->path().pointAtPercent(0);
        drag(w, end, {300, 180});
        auto found = std::find_if(w.project().wires.begin(), w.project().wires.end(),
                                  [&](const Wire &x) { return x.id == wire; });
        QVERIFY(found != w.project().wires.end());
        QVERIFY(found->from.object == d || found->to.object == d);
        auto final = encoded(w.project());
        QVERIFY(w.save_project(dir.filePath("wires.pds")));
        QVERIFY(w.open_project(dir.filePath("wires.pds")));
        QCOMPARE(encoded(w.project()), final);
    }
    void wire_click_cancel_invalid_and_preview() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        auto a = w.add_component(Kind::resistor, {0, 0}), b = w.add_component(Kind::resistor, {240, 0}),
             gate = w.add_pattern({0, 160});
        ready(w);
        auto *canvas = w.canvas();
        auto *vp = canvas->viewport();
        auto start = w.port_position({a, "n"});
        auto original = encoded(w.project());
        QTest::mouseClick(vp, Qt::LeftButton, Qt::NoModifier, canvas->mapFromScene(start));
        QCOMPARE(encoded(w.project()), original);
        QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, canvas->mapFromScene(start));
        QTest::mouseMove(vp, canvas->mapFromScene(QPointF(120, 80)), 5);
        QTest::keyClick(canvas, Qt::Key_Escape);
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, canvas->mapFromScene(QPointF(120, 80)));
        QCOMPARE(encoded(w.project()), original);
        drag(w, w.port_position({gate, "out"}), w.port_position({b, "p"}));
        QCOMPARE(encoded(w.project()), original);
        auto end = w.port_position({b, "p"});
        QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, canvas->mapFromScene(start));
        QTest::mouseMove(vp, canvas->mapFromScene(end), 5);
        QPainterPath preview;
        for (auto *i : canvas->scene()->items())
            if (i->zValue() == 100)
                preview = static_cast<QGraphicsPathItem *>(i)->path();
        QVERIFY(!preview.isEmpty());
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, canvas->mapFromScene(end));
        QCOMPARE(w.project().wires.size(), size_t(1));
        auto *wire = static_cast<QGraphicsPathItem *>(item(w, w.project().wires[0].id));
        QCOMPARE(wire->path(), preview);
    }
    void parameters_focus_and_profile() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        auto a = w.add_component(Kind::resistor, {0, 0}), b = w.add_component(Kind::capacitor, {240, 0});
        ready(w);
        QTest::mouseDClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                           w.canvas()->mapFromScene(QPointF(0, 0)));
        auto *field = w.findChild<QLineEdit *>("property_value");
        QCOMPARE(QApplication::focusWidget(), field);
        QTest::keyClick(field, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(field, "2kOhm");
        QTest::keyClick(field, Qt::Key_Return);
        QCOMPARE(w.project().components[0].value, 2000.0);
        QTest::keyClick(field, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(field, "invalid");
        QTest::keyClick(field, Qt::Key_Return);
        QCOMPARE(w.project().components[0].value, 2000.0);
        w.select_object(b);
        w.select_object(a);
        QCOMPARE(field->text(), QString("invalid"));
        QTest::keyClick(field, Qt::Key_Escape);
        QVERIFY(field->text() != QString("invalid"));
        QTest::keyClick(field, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(field, Qt::Key_Delete);
        QCOMPARE(w.project().components.size(), size_t(2));
        QTest::keyClick(field, Qt::Key_Escape);
        auto *stop = w.findChild<QLineEdit *>("sim_stop");
        stop->setFocus();
        QTest::keyClick(stop, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(stop, "10e-3");
        QTest::keyClick(stop, Qt::Key_Return);
        QCOMPARE(w.project().profile.stop, .01);
        w.select_object(b);
        QVERIFY(w.save_project(dir.filePath("profile.pds")));
        QVERIFY(w.open_project(dir.filePath("profile.pds")));
        QCOMPARE(w.project().profile.stop, .01);
    }
    void view_history_zoom_and_running_navigation() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        ready(w);
        w.set_scope_enabled(true);
        auto *channels = w.findChild<QListWidget *>("channels");
        QVERIFY(channels->count() > 0);
        channels->item(0)->setCheckState(Qt::Checked);
        w.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 10000);
        QVERIFY(w.has_result());
        auto id = w.project().components.front().id;
        w.select_object(id);
        auto original = item(w, id)->pos();
        w.canvas()->centerOn(original);
        drag(w, original, original + QPointF(40, 40));
        QTest::mouseClick(w.scope(), Qt::LeftButton, Qt::NoModifier, {200, 70});
        w.scope()->set_cursor_mode(true);
        w.scope()->set_cursor(0, .001);
        auto cursor = w.scope()->cursor_a;
        QVERIFY(cursor >= 0);
        w.undo();
        QCOMPARE(item(w, id)->pos(), original);
        QCOMPARE(w.scope()->cursor_a, cursor);
        QVERIFY(w.has_result());
        auto *canvas = w.canvas();
        auto at = QPoint(220, 120);
        auto anchor = canvas->mapToScene(at);
        QWheelEvent wheel(QPointF(at), canvas->viewport()->mapToGlobal(at), {}, QPoint(0, 30), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas->viewport(), &wheel);
        QVERIFY(QLineF(anchor, canvas->mapToScene(at)).length() < 2);
        QVERIFY(canvas->transform().m11() > 1 && canvas->transform().m11() < 1.15);
        auto *stop = w.findChild<QLineEdit *>("sim_stop");
        auto *step = w.findChild<QLineEdit *>("sim_step");
        stop->setText("1");
        step->setText("1e-8");
        w.start_simulation();
        QVERIFY(w.running());
        QVERIFY(canvas->isEnabled());
        auto old = canvas->transform();
        QApplication::sendEvent(canvas->viewport(), &wheel);
        QVERIFY(canvas->transform() != old);
        QTest::keyClick(stop, Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 10000);
    }
    void quick_insert_and_scaled_ports() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        ready(w);
        bool opened = false;
        QTimer::singleShot(30, &w, [&] {
            auto *dialog = w.findChild<QDialog *>("quick_insert");
            if (!dialog)
                return;
            opened = true;
            auto *search = dialog->findChild<QLineEdit *>("quick_search");
            QTest::keyClicks(search, "resistor");
            QTest::keyClick(search, Qt::Key_Return);
        });
        QTest::mouseDClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                           w.canvas()->mapFromScene(QPointF(40, 40)));
        QVERIFY(opened);
        QCOMPARE(w.project().components.size(), size_t(0));
        QTest::keyClick(w.canvas(), Qt::Key_Space);
        QTest::mouseClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                          w.canvas()->mapFromScene(QPointF(40, 40)));
        QCOMPARE(w.project().components.size(), size_t(1));
        QCOMPARE(w.project().components[0].orientation.quarter_turns, 1u);
        auto a = w.add_node(false, {0, 0}), b = w.add_node(false, {400, 0});
        w.canvas()->resetTransform();
        w.canvas()->scale(.2, .2);
        w.canvas()->centerOn(200, 0);
        auto *vp = w.canvas()->viewport();
        auto from = w.canvas()->mapFromScene(QPointF(0, 0)) + QPoint(0, 7),
             to = w.canvas()->mapFromScene(QPointF(400, 0)) + QPoint(0, 7);
        QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(vp, to, 5);
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, to);
        QCOMPARE(w.project().wires.size(), size_t(1));
        QCOMPARE(w.project().wires[0].from.object, a);
        QCOMPARE(w.project().wires[0].to.object, b);
    }
    void lost_capture_and_draft_events() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        auto a = w.add_component(Kind::resistor, {0, 0});
        auto g = w.add_pattern({240, 0});
        ready(w);
        auto original = encoded(w.project());
        auto *vp = w.canvas()->viewport();
        QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, w.canvas()->mapFromScene(QPointF(0, 0)));
        QTest::mouseMove(vp, w.canvas()->mapFromScene(QPointF(80, 80)), 5);
        QEvent lost(QEvent::UngrabMouse);
        QApplication::sendEvent(vp, &lost);
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, w.canvas()->mapFromScene(QPointF(80, 80)));
        QCOMPARE(encoded(w.project()), original);
        QCOMPARE(item(w, a)->pos(), QPointF(0, 0));
        w.select_object(g);
        auto *table = w.findChild<QTableWidget *>("property_events");
        table->setItem(0, 0, new QTableWidgetItem("5e-3"));
        table->setItem(0, 1, new QTableWidgetItem("invalid"));
        w.select_object(a);
        w.select_object(g);
        QCOMPARE(table->item(0, 1)->text(), QString("invalid"));
        table->item(0, 1)->setText("1");
        QTest::mouseClick(w.findChild<QPushButton *>("apply_properties"), Qt::LeftButton);
        QCOMPARE(w.project().events.size(), size_t(1));
        QCOMPARE(w.project().events[0].time, .005);
    }
    void benchmark_run_stop_responsiveness() {
        for (const auto *name : {"rc", "rlc", "vsi-2l", "npc-3l"}) {
            QTemporaryDir dir;
            EditorWindow w("en", dir.path());
            QVERIFY(w.open_project(QString(PDS_SOURCE_DIR "/examples/%1.pds").arg(name)));
            auto p = w.project();
            p.profile.stop = 1000;
            p.profile.step = 1e-6;
            w.set_project(p);
            ready(w);
            QElapsedTimer elapsed;
            elapsed.start();
            qint64 last_tick = 0, max_gap = 0;
            int ticks = 0;
            QTimer heartbeat;
            connect(&heartbeat, &QTimer::timeout, &w, [&] {
                auto now = elapsed.elapsed();
                max_gap = std::max(max_gap, now - last_tick);
                last_tick = now;
                ++ticks;
            });
            heartbeat.start(10);
            w.start_simulation();
            const auto dispatch = elapsed.elapsed();
            QVERIFY(w.running());
            QTest::qWait(120);
            // Exercise actual viewport input while the worker is computing.
            const QPoint center = w.canvas()->viewport()->rect().center();
            QTest::mousePress(w.canvas()->viewport(), Qt::MiddleButton, Qt::NoModifier, center);
            QTest::mouseMove(w.canvas()->viewport(), center + QPoint(20, 10), 0);
            QTest::mouseRelease(w.canvas()->viewport(), Qt::MiddleButton, Qt::NoModifier, center + QPoint(20, 10));
            QTest::qWait(60);
            QVERIFY(w.running());
            QVERIFY(ticks >= 3);
            QVERIFY2(max_gap < 1000, "UI must process events during the benchmark");
            QElapsedTimer stopping;
            stopping.start();
            w.stop_simulation();
            while (w.running() && stopping.elapsed() < 1000) QTest::qWait(1);
            QVERIFY2(!w.running(), "M1 benchmark Stop must complete within one second");
            const auto stop_ms = stopping.elapsed();
            QVERIFY(w.has_result());
            QVERIFY(w.result().cancelled);
            QVERIFY(w.result().last_time < p.profile.stop);
            for (size_t i = 1; i < w.result().samples.size(); ++i)
                QVERIFY(w.result().samples[i].time > w.result().samples[i - 1].time);
            qInfo().noquote() << QString("case=%1 dispatch=%2ms heartbeat_max_gap=%3ms stop=%4ms steps=%5 peak_resident=%6bytes")
                .arg(name).arg(dispatch).arg(max_gap).arg(stop_ms).arg(w.result().accepted_steps)
                .arg(benchmark::peak_resident_bytes());
        }
    }
    void performance() {
        if (!qEnvironmentVariableIsSet("PDS_UI_BENCHMARK"))
            QSKIP("Set PDS_UI_BENCHMARK for measured interaction timings");
        for (int count : {100, 500, 1000}) {
            Project p;
            p.id = new_uuid();
            p.wired = true;
            p.name = "Interaction benchmark";
            for (int i = 0; i < count; ++i)
                p.components.push_back({derived_uuid("ui/" + std::to_string(i)), "R" + std::to_string(i),
                                        Kind::resistor, "", "", 1000, 0, 240.0 * (i % 20), 160.0 * (i / 20)});
            for (int i = 0; i + 1 < count; ++i)
                if (i % 20 != 19)
                    p.wires.push_back({derived_uuid("wire/" + std::to_string(i)),
                                       {p.components[i].id, "n"},
                                       {p.components[i + 1].id, "p"},
                                       {}});
            QTemporaryDir dir;
            EditorWindow w("en", dir.path());
            QElapsedTimer init;
            init.start();
            w.set_project(p);
            ready(w);
            auto setup = init.elapsed();
            auto *canvas = w.canvas();
            auto *vp = canvas->viewport();
            std::vector<double> hover, move;
            auto measure = [&](std::vector<double> &samples, QPoint point) {
                QElapsedTimer t;
                t.start();
                QTest::mouseMove(vp, point, 0);
                QApplication::processEvents();
                samples.push_back(t.nsecsElapsed() / 1e6);
            };
            for (int i = 0; i < 90; ++i)
                measure(hover, canvas->mapFromScene(QPointF(80 + i, 90)));
            QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, canvas->mapFromScene(QPointF(0, 0)));
            for (int i = 0; i < 90; ++i)
                measure(move, canvas->mapFromScene(QPointF(20 + i % 60, 20 + i % 40)));
            QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier, canvas->mapFromScene(QPointF(80, 40)));
            auto stats = [](std::vector<double> v) {
                std::sort(v.begin(), v.end());
                return QString("p50=%1 p95=%2 max=%3 ms")
                    .arg(v[v.size() / 2], 0, 'f', 3)
                    .arg(v[v.size() * 95 / 100], 0, 'f', 3)
                    .arg(v.back(), 0, 'f', 3);
            };
            qInfo().noquote() << QString(
                                     "components=%1 wires=%2 window=%3x%4 DPR=%5 setup=%6ms hover %7 move %8")
                                     .arg(count)
                                     .arg(p.wires.size())
                                     .arg(w.width())
                                     .arg(w.height())
                                     .arg(w.devicePixelRatioF())
                                     .arg(setup)
                                     .arg(stats(hover), stats(move));
        }
        Project p;
        p.id = new_uuid(); p.name = "Hierarchy benchmark"; p.wired = true;
        Definition d;
        d.id = new_uuid(); d.name = "25 resistor section"; d.wired = true;
        for (int i = 0; i < 25; ++i)
            d.components.push_back({new_uuid(), "R" + std::to_string(i), Kind::resistor,
                                    "", "", 1000, 0, 240.0 * (i % 5), 160.0 * (i / 5)});
        p.definitions.push_back(d);
        for (int i = 0; i < 40; ++i)
            p.instances.push_back({new_uuid(), "Section " + std::to_string(i), d.id,
                                   240.0 * (i % 8), 200.0 * (i / 8)});
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        QElapsedTimer timer; timer.start();
        w.set_project(p); ready(w);
        const auto setup = timer.elapsed();
        qint64 maximum = 0;
        for (const auto &instance : p.instances) {
            timer.restart();
            w.navigate_hierarchy({instance.id});
            QApplication::processEvents();
            QCOMPARE(w.project().components.size(), size_t(25));
            w.navigate_hierarchy({});
            QApplication::processEvents();
            maximum = std::max(maximum, timer.elapsed());
        }
        qInfo() << "hierarchy: 40 instances / 1000 expanded atoms, setup" << setup
                << "ms, max open-and-return" << maximum << "ms";
    }
    void simulation_performance() {
        const auto path = qEnvironmentVariable("PDS_SIM_BENCHMARK_PROJECT");
        if (path.isEmpty())
            QSKIP("Set PDS_SIM_BENCHMARK_PROJECT to measure the desktop simulation lifecycle");
        QTemporaryDir dir;
        QElapsedTimer timer;
        timer.start();
        EditorWindow w("en", dir.path());
        const auto construction = timer.elapsed();
        QVERIFY(w.open_project(path));
        auto p = w.project();
        p.scope_enabled = true;
        QVERIFY(!p.plots.empty());
        p.scope_channels = plot_channels(p, p.plots.front().id);
        w.set_project(p);
        w.show();
        w.findChild<QTabWidget *>("results_tabs")->setCurrentIndex(1);
        w.open_plot(p.plots.front().id);
        QApplication::processEvents();
        const auto setup = timer.elapsed();
        const auto screenshot = qEnvironmentVariable("PDS_SIM_SCREENSHOT_PATH");
        if (!screenshot.isEmpty())
            QVERIFY(w.grab().save(screenshot));
        for (int run = 0; run < 2; ++run) {
            timer.restart();
            w.start_simulation();
            const auto dispatch = timer.elapsed();
            QVERIFY2(dispatch < 100, "Starting simulation must not wait for old result destruction");
            QVERIFY(w.running());
            QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 15000);
            const auto ready_time = timer.elapsed();
            QVERIFY(w.has_result());
            QCOMPARE(w.result().last_time, p.profile.stop);
            w.scope()->fit();
            QApplication::processEvents();
            const auto shown = timer.elapsed();
            (void)w.scope()->grab();
            const auto scope_paint = timer.elapsed() - shown;
            qInfo().noquote()
                << QString("constructor=%1 ms setup=%2 ms dispatch=%3 ms full_run=%4 ms steps=%5 channels=%6")
                       .arg(construction)
                       .arg(setup)
                       .arg(dispatch)
                       .arg(timer.elapsed())
                       .arg(w.result().accepted_steps)
                       .arg(w.result().channels.size());
            qInfo() << "ready" << ready_time << "fit_and_events" << shown - ready_time << "scope_paint"
                    << scope_paint;
            bool elapsed = false;
            for (auto *label : w.findChildren<QLabel *>())
                if (label->text().contains("elapsed")) {
                    elapsed = true;
                    qInfo().noquote() << label->text() << label->toolTip();
                }
            QVERIFY(elapsed);
        }
    }
};
QTEST_MAIN(InteractionTests)
#include "interaction_tests.moc"
