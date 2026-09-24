#include "apps/desktop/editor.hpp"
#include "apps/desktop/code_editor.hpp"
#include "apps/desktop/code_icon_editor.hpp"
#include "apps/desktop/signal_presets.hpp"
#include "core/model/c_program.hpp"
#include "core/model/hierarchy.hpp"
#include "core/editor/properties.hpp"
#include "formats/project/project.hpp"
#include "formats/snapshot/snapshot.hpp"
#include "tests/qt_test_main.hpp"
#include <tuple>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QProgressBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QWheelEvent>
#include <QtTest/QtTest>
#include <array>
#include <cmath>
#include <fstream>
#include <sstream>
using namespace pds;
using namespace pds::desktop;
class DesktopTests : public QObject {
    Q_OBJECT
  private slots:
    void dc_motor_palette_properties_and_example() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        EditorWindow window("en",temp.path());
        window.show();
        QVERIFY(window.findChild<QAction *>("insert_component_12"));
        const auto id=window.add_component(Kind::dc_motor,{0,0});
        QVERIFY(!id.empty());
        QCOMPARE(window.project().components.front().value,2.0);
        auto edited=window.project();
        write_property(edited,id,"motor_torque_constant",0.2);
        QCOMPARE(edited.components.front().motor.torque_constant,0.2);
        QCOMPARE(edited.components.front().motor.back_emf_constant,0.2);
        window.set_project(edited);
        EditorWindow example("en",temp.path());
        QVERIFY(example.open_project(QString(PDS_SOURCE_DIR)+"/examples/dc-motor-startup.pds"));
        QCOMPARE(example.project().components.size(),size_t(2));
        QCOMPARE(example.project().components[1].kind,Kind::dc_motor);
        const auto path=temp.filePath("dc-motor-copy.pds");
        QVERIFY(example.save_project(path));
        QVERIFY(example.open_project(path));
        if(const auto screenshot=qEnvironmentVariable("PDS_MOTOR_SCREENSHOT");!screenshot.isEmpty()) {
            example.resize(1280,820);
            example.show();
            example.findChild<QAction *>("action_fit")->trigger();
            QTest::qWait(80);
            QVERIFY(example.grab().save(screenshot));
        }
        example.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!example.running(),5000);
        QVERIFY(example.has_result());
        const auto speed=std::find_if(example.result().channels.begin(),example.result().channels.end(),
                                      [](const Channel &channel){return channel.name=="omega:DC motor";});
        QVERIFY(speed!=example.result().channels.end());
        QVERIFY(!example.result().samples.empty());
        const auto index=size_t(speed-example.result().channels.begin());
        QVERIFY(std::abs(example.result().samples.back().values[index]-76.016)<0.01);
    }
    void ideal_transformer_palette_and_ports() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        EditorWindow window("en",temp.path());
        window.show();
        auto *action=window.findChild<QAction *>("insert_component_11");
        QVERIFY(action);
        const auto id=window.add_component(Kind::ideal_transformer,{0,0});
        QVERIFY(!id.empty());
        QCOMPARE(window.project().components.front().value,1.0);
        for(const auto &[name,position]:std::vector<std::pair<QString,QPointF>>{
                {"p",{-60,-20}},{"n",{-60,20}},{"sp",{60,-20}},{"sn",{60,20}}}) {
            auto found=false;
            for(auto *item:window.canvas()->scene()->items())
                if(item->data(0).toString().toStdString()==id && item->data(2).toString()==name) {
                    QCOMPARE(item->pos(),position);
                    found=true;
                }
            QVERIFY(found);
        }
        EditorWindow example("en",temp.path());
        QVERIFY(example.open_project(QString(PDS_SOURCE_DIR)+"/examples/ideal-transformer.pds"));
        QCOMPARE(example.project().components.size(),size_t(3));
        QCOMPARE(example.project().wires.size(),size_t(6));
        if(const auto screenshot=qEnvironmentVariable("PDS_TRANSFORMER_SCREENSHOT");!screenshot.isEmpty()) {
            example.resize(1280,820);
            example.show();
            example.findChild<QAction *>("action_fit")->trigger();
            QTest::qWait(80);
            QVERIFY(example.grab().save(screenshot));
        }
        const auto path=temp.filePath("transformer-copy.pds");
        QVERIFY(example.save_project(path));
        QVERIFY(example.open_project(path));
        QCOMPARE(example.project().components[1].kind,Kind::ideal_transformer);
        example.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!example.running(),5000);
        QVERIFY(example.has_result());
        QVERIFY(example.result().max_scaled_residual<1e-12);
    }
    void code_editor_find_replace_and_help() {
        init_language("en");
        CCodeEdit editor(false);editor.resize(600,400);editor.setPlainText("alpha beta alpha");editor.show();editor.setFocus();
        QTimer::singleShot(50,[&] {
            auto *dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget());QVERIFY(dialog);
            QCOMPARE(dialog->objectName(),QString("code_find_dialog"));
            dialog->findChild<QLineEdit *>("code_find_text")->setText("beta");
            dialog->findChild<QPushButton *>("code_find_next")->click();
            dialog->accept();
        });
        QKeyEvent find_event(QEvent::KeyPress,Qt::Key_F,Qt::ControlModifier);
        QApplication::sendEvent(&editor,&find_event);
        QCOMPARE(editor.textCursor().selectedText(),QString("beta"));
        QTimer::singleShot(50,[&] {
            auto *dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget());QVERIFY(dialog);
            QCOMPARE(dialog->objectName(),QString("code_replace_dialog"));
            dialog->findChild<QLineEdit *>("code_find_text")->setText("alpha");
            dialog->findChild<QLineEdit *>("code_replace_text")->setText("gamma");
            dialog->findChild<QPushButton *>("code_replace_all")->click();
            dialog->accept();
        });
        QKeyEvent replace_event(QEvent::KeyPress,Qt::Key_H,Qt::ControlModifier);
        QApplication::sendEvent(&editor,&replace_event);
        QCOMPARE(editor.toPlainText(),QString("gamma beta gamma"));
        QTimer::singleShot(50,[&] {
            auto *dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget());QVERIFY(dialog);
            QCOMPARE(dialog->objectName(),QString("c_code_reference_dialog"));
            QVERIFY(dialog->findChild<QTextBrowser *>("c_code_reference_view"));dialog->accept();
        });
        QKeyEvent help_event(QEvent::KeyPress,Qt::Key_F1,Qt::NoModifier);
        QApplication::sendEvent(&editor,&help_event);
    }

    void icon_editor_previews_and_snaps_to_configurable_grid() {
        init_language("en");CodeIconEditor editor({});editor.resize(640,520);editor.show();
        auto *grid=editor.findChild<QSpinBox *>("code_icon_grid_step");QVERIFY(grid);grid->setValue(8);
        auto *line=editor.findChild<QToolButton *>("icon_line");
        QVERIFY(line);QTest::mouseClick(line,Qt::LeftButton);
        const auto before=editor.grab().toImage();
        QTest::mousePress(&editor,Qt::LeftButton,Qt::NoModifier,{120,90});
        QTest::mouseMove(&editor,{500,390});QApplication::processEvents();
        QVERIFY(editor.icon().empty());
        const auto during=editor.grab().toImage();QVERIFY(during!=before);
        QTest::mouseRelease(&editor,Qt::LeftButton,Qt::NoModifier,{500,390});
        QCOMPARE(editor.icon().size(),size_t(1));
        for(const auto &point:editor.icon().front().points) {
            QCOMPARE(std::fmod(point.x,8.0),0.0);QCOMPARE(std::fmod(point.y,8.0),0.0);
        }
        std::vector<IconPrimitive> full(max_icon_primitives,
            {IconPrimitiveKind::line,IconColor::foreground,false,{},{{0,0},{32,32}}});
        editor.set_icon(full);
        QTest::mousePress(&editor,Qt::LeftButton,Qt::NoModifier,{120,90});
        QTest::mouseRelease(&editor,Qt::LeftButton,Qt::NoModifier,{500,390});
        QCOMPARE(editor.icon().size(),max_icon_primitives);
        editor.set_icon({});
        auto *snap=editor.findChild<QCheckBox *>("code_icon_snap");QVERIFY(snap);snap->setChecked(false);
        auto *freehand=editor.findChild<QToolButton *>("icon_freehand");
        QVERIFY(freehand);QTest::mouseClick(freehand,Qt::LeftButton);
        QTest::mousePress(&editor,Qt::LeftButton,Qt::NoModifier,{110,100});
        for(int index=1;index<320;++index) {
            const QPointF point(110+(index%50)*8,100+(index/50)*45);
            QMouseEvent move(QEvent::MouseMove,point,editor.mapToGlobal(point.toPoint()),
                             Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
            QApplication::sendEvent(&editor,&move);
        }
        QTest::mouseRelease(&editor,Qt::LeftButton,Qt::NoModifier,{262,370});
        QCOMPARE(editor.icon().size(),size_t(1));
        QCOMPARE(editor.icon().front().points.size(),max_icon_points);
    }

    void object_icons_are_editable_from_properties() {
        QTemporaryDir temp;EditorWindow window("en",temp.path());window.show();
        const auto resistor=window.add_component(Kind::resistor,{120,120});window.select_object(resistor);
        auto *edit=window.findChild<QPushButton *>("edit_object_icon");QVERIFY(edit);
        QTimer::singleShot(50,[&] {
            auto *dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget());QVERIFY(dialog);
            auto *canvas=dynamic_cast<CodeIconEditor *>(dialog->findChild<QWidget *>("code_icon_canvas"));QVERIFY(canvas);
            QCOMPARE(canvas->icon(),editable_component_icon(0));
            auto *grid=dialog->findChild<QSpinBox *>("code_icon_grid_step");QVERIFY(grid);grid->setValue(2);
            QVERIFY(dialog->findChild<QCheckBox *>("code_icon_snap")->isChecked());
            canvas->set_icon({{IconPrimitiveKind::line,IconColor::accent,false,{},{{4,4},{28,28}}}});
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        edit->click();
        QCOMPARE(window.project().object_icons.size(),size_t(1));
        QCOMPARE(window.project().object_icons.front().object,resistor);
        window.undo();QVERIFY(window.project().object_icons.empty());
        window.redo();QCOMPARE(window.project().object_icons.size(),size_t(1));
        const auto path=temp.filePath("object-icon.pds");QVERIFY(window.save_project(path));QVERIFY(window.open_project(path));
        QCOMPARE(window.project().object_icons.size(),size_t(1));
    }

    void differential_plot_records_difference() {
        QTemporaryDir temp;
        Project project; project.id=new_uuid(); project.wired=true; project.profile={1e-4,1e-5};
        Document document(project);
        const auto ground=document.add_node(true,0,200);
        const auto first=document.add_component(Kind::voltage,0,0);
        const auto second=document.add_component(Kind::voltage,0,100);
        const auto positive=document.add_component(Kind::voltage_probe,180,0);
        const auto negative=document.add_component(Kind::voltage_probe,180,100);
        const auto graph=document.add_plot(360,50,"Differential",true);
        document.apply("Voltages",[&](Project& p){p.components[0].value=5;p.components[1].value=2;});
        document.connect({first,"n"},{ground,"node"});
        document.connect({second,"n"},{ground,"node"});
        document.connect({positive,"p"},{first,"p"});
        document.connect({positive,"n"},{ground,"node"});
        document.connect({negative,"p"},{second,"p"});
        document.connect({negative,"n"},{ground,"node"});
        document.connect({positive,"out"},{graph,"p1"});
        document.connect({negative,"out"},{graph,"n1"});
        EditorWindow window("en",temp.path());
        window.set_project(document.project());
        window.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!window.running(),5000);
        QVERIFY(window.has_result());
        const auto key="diff/"+graph+"/1";
        const auto channel=std::find_if(window.result().channels.begin(),window.result().channels.end(),
                                        [&](const Channel& value){return value.object==key;});
        QVERIFY(channel!=window.result().channels.end());
        const auto index=size_t(channel-window.result().channels.begin());
        QVERIFY(std::abs(window.result().samples.back().values[index]-3.0)<1e-12);
    }
    void create_connect_edit_run_save_recover() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        EditorWindow window("ru", temp.path());
        window.show();
        QTest::qWait(50);
        auto *library = window.findChild<QTreeWidget *>("library");
        QVERIFY(library);
        QCOMPARE(library->topLevelItemCount(), 6);
        QVERIFY(window.scope() == nullptr);
        QVERIFY(!window.project().scope_enabled);
        auto place = [&](int kind, QPointF point) {
            QTreeWidgetItem *chosen = nullptr;
            for (QTreeWidgetItemIterator it(library); *it; ++it)
                if ((*it)->data(0, Qt::UserRole).isValid() && (*it)->data(0, Qt::UserRole).toInt() == kind)
                    chosen = *it;
            if (!chosen) {
                auto *bar = window.findChild<QToolBar *>("component_bar");
                auto *action = window.findChild<QAction *>("insert_component_" + QString::number(kind));
                QVERIFY(action && bar->actions().contains(action));
                QTest::mouseClick(bar->widgetForAction(action), Qt::LeftButton);
            } else {
                for (auto *parent = chosen->parent(); parent; parent = parent->parent()) parent->setExpanded(true);
                library->scrollToItem(chosen);
                QTest::mouseClick(library->viewport(), Qt::LeftButton, Qt::NoModifier,
                                  library->visualItemRect(chosen).center());
                QTest::mouseDClick(library->viewport(), Qt::LeftButton, Qt::NoModifier,
                                   library->visualItemRect(chosen).center());
            }
            QTest::mouseClick(window.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                              window.canvas()->mapFromScene(point));
        };
        place(3, {-40, 20});
        place(0, {200, 20});
        place(1, {200, 180});
        place(100, {-40, 280});
        QCOMPARE(window.project().components.size(), size_t(3));
        QCOMPARE(window.project().nodes.size(), size_t(1));
        window.findChild<QAction *>("action_fit")->trigger();
        auto v = window.project().components[0].id, r = window.project().components[1].id,
             c = window.project().components[2].id, g = window.project().nodes[0].id;
        auto wire = [&](Endpoint a, Endpoint b) {
            QTest::mousePress(window.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                              window.canvas()->mapFromScene(window.port_position(a)));
            QTest::mouseMove(window.canvas()->viewport(),
                             window.canvas()->mapFromScene(window.port_position(b)));
            QTest::mouseRelease(window.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                                window.canvas()->mapFromScene(window.port_position(b)));
        };
        wire({v, "p"}, {r, "p"});
        wire({r, "n"}, {c, "p"});
        wire({v, "n"}, {g, "node"});
        wire({c, "n"}, {g, "node"});
        QCOMPARE(window.project().wires.size(), size_t(4));
        window.select_object(r);
        auto *value = window.findChild<QLineEdit *>("property_value");
        auto *apply = window.findChild<QPushButton *>("apply_properties");
        value->setText("2kOhm");
        QTest::mouseClick(apply, Qt::LeftButton);
        QCOMPARE(window.project().components[1].value, 2000.0);
        window.undo();
        QCOMPARE(window.project().components[1].value, 1000.0);
        window.redo();
        QCOMPARE(window.project().components[1].value, 2000.0);
        window.undo();
        value = window.findChild<QLineEdit *>("property_value");
        apply = window.findChild<QPushButton *>("apply_properties");
        value->setText("not a resistance");
        QTest::mouseClick(apply, Qt::LeftButton);
        QCOMPARE(window.project().components[1].value, 1000.0);
        auto *diagnostics = window.findChild<QListWidget *>("diagnostics_list");
        QVERIFY(diagnostics && diagnostics->count() > 0);
        diagnostics->setCurrentRow(0);
        diagnostics->setFocus();
        QApplication::clipboard()->setText("previous clipboard contents");
        QTest::keyClick(diagnostics, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), diagnostics->item(0)->text());
        window.observe_object(window.project().wires[1].id);
        window.observe_object(v);
        QVERIFY(window.scope() != nullptr);
        window.findChild<QLineEdit *>("sim_stop")->setText("5ms");
        window.findChild<QLineEdit *>("sim_step")->setText("1us");
        window.findChild<QAction *>("action_run")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!window.running(), 10000);
        QVERIFY(window.has_result());
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
        window.scope()->set_cursor_mode(true);
        window.scope()->set_cursor(0, .001);
        window.scope()->set_cursor(1, .004);
        QVERIFY(window.project().cursor_a >= 0);
        QVERIFY(window.project().cursor_b > window.project().cursor_a);
        const auto screenshot = qEnvironmentVariable("PDS_SCREENSHOT_PATH");
        if (!screenshot.isEmpty()) {
            window.resize(1600, 1000);
            auto *results = window.findChild<QDockWidget *>("results");
            auto *inspector = window.findChild<QDockWidget *>("inspector");
            QVERIFY(results);
            QVERIFY(inspector);
            window.resizeDocks({results}, {420}, Qt::Vertical);
            window.resizeDocks({inspector}, {350}, Qt::Horizontal);
            QTest::qWait(50);
            window.select_object(c);
            QTest::qWait(50);
            window.findChild<QAction *>("action_fit")->trigger();
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
        QTRY_VERIFY_WITH_TIMEOUT(!window.running(), 5000);
        QVERIFY(window.has_result());
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
        const auto gate_wire = window.project().wires.back().id;
        window.select_object(pattern);
        window.findChild<QCheckBox *>("property_closed")->setChecked(true);
        auto *events = window.findChild<QTableWidget *>("property_events");
        QVERIFY(events->toolTip().contains("Ctrl+V"));
        events->setCurrentCell(0, 0);
        QApplication::clipboard()->setText("{{1e-3, 0}, {2e-3, 1}}");
        QTest::keyClick(events, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(events->item(0, 0)->text(), QString("1e-3"));
        QCOMPARE(events->item(0, 1)->text(), QString("0"));
        QCOMPARE(events->item(1, 0)->text(), QString("2e-3"));
        QCOMPARE(events->item(1, 1)->text(), QString("1"));
        QCOMPARE(window.findChild<QComboBox *>("property_gate_mode")->currentText(), QString("Table"));
        QTest::mouseClick(window.findChild<QPushButton *>("apply_properties"), Qt::LeftButton);
        QCOMPARE(window.project().events.size(), size_t(2));
        window.observe_object(vp);
        window.observe_object(ip);
        window.observe_object(gate_wire);
        auto *selected_channels = window.findChild<QListWidget *>("channels");
        for (int i = 0; i < selected_channels->count(); ++i) {
            auto *item = selected_channels->item(i);
            auto key = item->data(Qt::UserRole).toString().toStdString();
            item->setCheckState(key == vp || key == ip || key == "gate/" + sw ? Qt::Checked : Qt::Unchecked);
        }
        window.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!window.running(), 10000);
        QVERIFY(window.has_result());
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
        QTRY_VERIFY_WITH_TIMEOUT(!window.running(), 10000);
        QVERIFY(window.has_result());
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
        av->set_cursor_mode(true);
        QTest::mouseClick(av, Qt::LeftButton, Qt::NoModifier, {260, 100});
        QVERIFY(window.project().plots[0].cursor_a >= 0);
        QCOMPARE(window.project().plots[1].cursor_a, -1.0);
        auto *legend = a->findChild<QToolBar *>("plot_legend");
        QVERIFY(legend && legend->actions().size() == 1);
        const auto samples = window.result().samples.size();
        QTest::mouseClick(
            legend->widgetForAction(legend->actions().front())->findChild<QToolButton *>("curve_visibility"),
            Qt::LeftButton, Qt::NoModifier, QPoint(10, 10));
        QVERIFY(!av->channel_visible(probe));
        QVERIFY(bv->channel_visible(probe));
        QCOMPARE(window.result().samples.size(), samples);
        auto options =
            std::find_if(window.project().view_options.begin(), window.project().view_options.end(),
                         [&](const ViewOptions &v) { return v.plot == first; });
        QVERIFY(options != window.project().view_options.end());
        QCOMPARE(options->hidden_channels, std::vector<std::string>{probe});
        a->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        window.open_plot(first);
        a = window.findChild<QDialog *>("plot_" + QString::fromStdString(first));
        QVERIFY(a);
        av = a->findChild<Scope *>("scope");
        QVERIFY(!av->channel_visible(probe));
        av->set_channel_visible(probe, true);
        const auto screenshot = qEnvironmentVariable("PDS_PLOT_SCREENSHOT_PATH");
        if (!screenshot.isEmpty()) {
            QTest::qWait(40);
            QVERIFY(a->grab().save(screenshot));
        }
        a->close();
        b->close();
        window.observe_object(window.project().wires.front().id);
        window.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!window.running(), 10000);
        QVERIFY(window.has_result());
        QVERIFY(window.result().channels.size() > 1);
        window.set_scope_enabled(false);
        QVERIFY(window.scope() == nullptr);
        QCOMPARE(window.result().channels.size(), size_t(1));
        window.select_object(first);
        window.findChild<QAction *>("action_delete")->trigger();
        window.select_object(second);
        window.findChild<QAction *>("action_delete")->trigger();
        window.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!window.running(), 10000);
        QVERIFY(window.has_result());
        QVERIFY(window.result().samples.empty());
        QVERIFY(window.result().channels.empty());
    }
    void experiment_dialog_runs_sweep() {
        QTemporaryDir temp;
        EditorWindow window("ru", temp.path());
        window.show();
        QVERIFY(window.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc-sweep.pds"));
        QCOMPARE(window.root_project().experiments.size(), size_t(1));
        bool checked = false;
        std::vector<int> live_progress;
        QTimer::singleShot(80, [&] {
            auto *dialog = window.findChild<QDialog *>("experiments_dialog");
            QVERIFY(dialog);
            auto *list = dialog->findChild<QListWidget *>("experiments_list");
            QVERIFY(list);
            QCOMPARE(list->count(), 1);
            auto *run = dialog->findChild<QPushButton *>("experiment_run");
            QVERIFY(run);
            auto *progress = dialog->findChild<QProgressBar *>("experiment_progress");
            QVERIFY(progress);
            connect(progress, &QProgressBar::valueChanged, dialog,
                    [&](int value) { live_progress.push_back(value); });
            QTest::mouseClick(run, Qt::LeftButton);
        });
        QTimer::singleShot(1400, [&] {
            auto *dialog = window.findChild<QDialog *>("experiments_dialog");
            QVERIFY(dialog);
            auto *cases = dialog->findChild<QTableWidget *>("experiment_cases");
            QVERIFY(cases);
            QCOMPARE(cases->rowCount(), 6);
            for (int row = 0; row < cases->rowCount(); ++row)
                QVERIFY(cases->item(row, 5)->text().isEmpty());
            QVERIFY(std::any_of(live_progress.begin(), live_progress.end(),
                                [](int value) { return value > 0 && value < 6; }));
            checked = true;
            dialog->accept();
        });
        window.show_experiments();
        QVERIFY(checked);
        QCOMPARE(window.root_project().experiments.size(), size_t(1));
    }
    void observed_scope_points_remain_until_deleted() {
        QTemporaryDir temp;
        EditorWindow window("en", temp.path());
        window.show();
        QVERIFY(window.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        window.observe_object(window.project().wires.front().id);
        auto *channels = window.findChild<QListWidget *>("channels");
        QVERIFY(channels);
        QCOMPARE(channels->count(), 1);
        QCOMPARE(window.project().scope_points.size(), size_t(1));
        channels->item(0)->setCheckState(Qt::Unchecked);
        QCOMPARE(channels->count(), 1);
        QCOMPARE(window.project().scope_points.size(), size_t(1));
        QVERIFY(window.project().scope_channels.empty());
        channels->setCurrentRow(0);
        QTest::keyClick(channels, Qt::Key_Delete);
        QCOMPARE(channels->count(), 0);
        QVERIFY(window.project().scope_points.empty());
        QVERIFY(window.project().scope_channels.empty());
    }
    void deleting_disconnected_current_observation_is_safe() {
        QTemporaryDir temp;
        EditorWindow window("en", temp.path());
        window.show();
        QVERIFY(window.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));

        const auto observed_wire = window.project().wires.front().id;
        window.observe_wire_current(observed_wire);
        QCOMPARE(window.project().scope_points.size(), size_t(1));
        const auto probe = window.project().scope_points.front();
        QCOMPARE(std::count_if(window.project().wires.begin(), window.project().wires.end(),
                               [&](const Wire &wire) {
                                   return wire.from.object == probe || wire.to.object == probe;
                               }),
                 2);

        // Load an already damaged observation created by an older editor version.
        // Current wire deletion removes the hidden probe atomically.
        auto damaged = window.project();
        damaged.wires.erase(std::remove_if(damaged.wires.begin(), damaged.wires.end(),
                                           [&](const Wire &wire) { return wire.id == observed_wire; }),
                            damaged.wires.end());
        window.set_project(std::move(damaged));
        QCOMPARE(window.project().scope_points.size(), size_t(1));
        QCOMPARE(std::count_if(window.project().wires.begin(), window.project().wires.end(),
                               [&](const Wire &wire) {
                                   return wire.from.object == probe || wire.to.object == probe;
                               }),
                 1);

        auto *channels = window.findChild<QListWidget *>("channels");
        QVERIFY(channels);
        QCOMPARE(channels->count(), 1);
        channels->setCurrentRow(0);
        QTest::keyClick(channels, Qt::Key_Delete);

        QVERIFY(window.project().scope_points.empty());
        QVERIFY(window.project().scope_channels.empty());
        QVERIFY(std::none_of(window.project().components.begin(), window.project().components.end(),
                             [&](const Component &component) { return component.id == probe; }));
        QVERIFY(std::none_of(window.project().wires.begin(), window.project().wires.end(),
                             [&](const Wire &wire) {
                                 return wire.from.object == probe || wire.to.object == probe;
                             }));
        auto *diagnostics = window.findChild<QListWidget *>("diagnostics_list");
        QVERIFY(diagnostics);
        QVERIFY(diagnostics->count() >= 1);
        QCOMPARE(diagnostics->item(diagnostics->count() - 1)->data(Qt::UserRole + 2).toString(),
                 QString("warning"));
        bool valid = true;
        try {
            (void)resolve_connections(window.root_project());
        } catch (...) {
            valid = false;
        }
        QVERIFY(valid);

        window.undo();
        QVERIFY(std::find(window.project().scope_points.begin(), window.project().scope_points.end(), probe) !=
                window.project().scope_points.end());
        QCOMPARE(std::count_if(window.project().wires.begin(), window.project().wires.end(),
                               [&](const Wire &wire) {
                                   return wire.from.object == probe || wire.to.object == probe;
                               }),
                 1);
        window.redo();
        QVERIFY(std::find(window.project().scope_points.begin(), window.project().scope_points.end(), probe) ==
                window.project().scope_points.end());
        QVERIFY(std::none_of(window.project().wires.begin(), window.project().wires.end(),
                             [&](const Wire &wire) {
                                 return wire.from.object == probe || wire.to.object == probe;
                             }));
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
        window.canvas()->centerOn(100, 160);
        QTest::mousePress(window.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                          window.canvas()->mapFromScene(window.port_position({resistor, "p"})));
        QTest::mouseMove(window.canvas()->viewport(), window.canvas()->mapFromScene(QPointF(100, 220)));
        QTest::mouseRelease(window.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                            window.canvas()->mapFromScene(QPointF(100, 220)));
        QCOMPARE(window.project().nodes.size(), size_t(2));
        QCOMPARE(window.project().wires.size(), size_t(1));
        window.select_object(window.project().wires.front().id);
        QVERIFY(!window.findChild<QPlainTextEdit *>("property_bends"));
        auto *color = window.findChild<QPushButton *>("property_wire_color");
        auto *width = window.findChild<QLineEdit *>("property_wire_width");
        auto *line = window.findChild<QComboBox *>("property_wire_line");
        QVERIFY(color && width && line);
        color->setProperty("color_value", "#c04080");
        color->setProperty("draft", true);
        width->setText("3.5");
        width->setModified(true);
        line->setCurrentIndex(line->findData(unsigned(WireLine::dash)));
        line->setProperty("draft", true);
        QTest::mouseClick(window.findChild<QPushButton *>("apply_properties"), Qt::LeftButton);
        QCOMPARE(window.project().wires.front().color, std::string("#c04080"));
        QCOMPARE(window.project().wires.front().width, 3.5);
        QCOMPARE(window.project().wires.front().line, WireLine::dash);
        const auto wire_screenshot = qEnvironmentVariable("PDS_WIRE_PROPERTY_SCREENSHOT");
        if (!wire_screenshot.isEmpty()) {
            QTest::qWait(30);
            QVERIFY(window.grab().save(wire_screenshot));
        }
        window.stop_simulation();
    }
    void gestures_taps_and_transforms() {
        QTemporaryDir temp;
        EditorWindow window("ru", temp.path());
        window.show();
        QTest::qWait(30);
        auto r = window.add_component(Kind::resistor, {100, 80});
        auto c = window.add_component(Kind::capacitor, {380, 80});
        auto plot = window.add_plot({320, 240});
        QVERIFY(window.connect_ports({r, "n"}, {c, "p"}));
        auto wire = window.project().wires.front().id;
        QPointF middle;
        for (auto *item : window.canvas()->scene()->items())
            if (item->data(0).toString().toStdString() == wire) {
                auto *path = dynamic_cast<QGraphicsPathItem *>(item);
                if (path)
                    middle = path->path().pointAtPercent(.5);
            }
        auto *viewport = window.canvas()->viewport();
        auto from = window.canvas()->mapFromScene(window.port_position({plot, "in1"}));
        auto end = window.canvas()->mapFromScene(middle);
        QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(viewport, end);
        QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, end);
        QCOMPARE(window.project().wires.size(), size_t(3));
        QCOMPARE(window.project().nodes.size(), size_t(1));
        QCOMPARE(plot_channels(window.project(), plot).size(), size_t(1));
        window.undo();
        QCOMPARE(window.project().wires.size(), size_t(1));
        window.redo();
        window.select_object(r);
        auto before = window.port_position({r, "p"});
        window.findChild<QAction *>("action_rotate")->trigger();
        auto after = window.port_position({r, "p"});
        QVERIFY(before != after);
        QCOMPARE(after, QPointF(100, 20));
        window.findChild<QAction *>("action_mirror")->trigger();
        QVERIFY(window.project().components.front().orientation.mirrored);
        window.findChild<QAction *>("action_copy")->trigger();
        window.findChild<QAction *>("action_paste")->trigger();
        QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier,
                          window.canvas()->mapFromScene(QPointF(400, 300)));
        QCOMPARE(window.project().components.size(), size_t(3));
        QVERIFY(window.save_project(temp.filePath("edited.pds")));
        QVERIFY(window.open_project(temp.filePath("edited.pds")));
        QVERIFY(window.project().components.front().orientation.mirrored);
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
    void code_block_editor_keeps_connected_port_identity() {
        QTemporaryDir temp;
        Project project; project.id = new_uuid(); project.wired = true; project.schema = project_schema;
        CodeBlock source;
        source.id = new_uuid(); source.name = "Source"; source.x = 0; source.y = 80;
        source.period = 1e-4; source.code = "value = 1;";
        source.outputs.push_back({new_uuid(), "value", "V", SignalScalarType::real, 0});
        CodeBlock sink;
        sink.id = new_uuid(); sink.name = "Sink"; sink.x = 300; sink.y = 80;
        sink.period = 1e-4; sink.code = "result = input;";
        sink.inputs.push_back({new_uuid(), "input", "V", SignalScalarType::real, 0});
        sink.inputs.push_back({new_uuid(), "aux", "V", SignalScalarType::real, 0});
        sink.outputs.push_back({new_uuid(), "result", "V", SignalScalarType::real, 0});
        project.code_blocks = {source, sink};
        project.wires.push_back({new_uuid(), {source.id, source.outputs[0].id},
                                 {sink.id, sink.inputs[0].id}, {}});
        const auto input_id = sink.inputs[0].id;
        EditorWindow window("en", temp.path());
        window.set_project(project);
        window.show();
        QCOMPARE(window.statusBar()->currentMessage(), QString("2 elements · 1 wires"));
        QGraphicsItem *sink_item = nullptr;
        for (auto *item : window.canvas()->scene()->items())
            if (item->data(0).toString().toStdString() == sink.id && item->data(1).toString() == "atom")
                sink_item = item;
        QVERIFY(sink_item);
        QCOMPARE(sink_item->shape().boundingRect().size(), QSizeF(140, 56));
        window.select_object(sink.id);
        const auto screenshot = qEnvironmentVariable("PDS_CODE_BLOCK_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            window.findChild<QAction *>("action_fit")->trigger();
            QTest::qWait(50);
            QVERIFY(window.grab().save(screenshot));
        }
        QVERIFY(window.findChild<QPushButton *>("edit_code_block"));
        QVERIFY(!window.port_position({sink.id, input_id}).isNull());
        QTimer first_safety;
        first_safety.setSingleShot(true);
        connect(&first_safety, &QTimer::timeout, [&] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
        });
        first_safety.start(3000);
        QTimer::singleShot(50, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *tabs=dialog->findChild<QTabWidget *>("code_block_tabs");QVERIFY(tabs);
            QCOMPARE(tabs->count(),4);QCOMPARE(tabs->tabText(0),QString("Code"));
            QCOMPARE(tabs->tabText(1),QString("Inputs"));QCOMPARE(tabs->tabText(2),QString("Outputs"));
            QCOMPARE(tabs->tabText(3),QString("Icon"));QCOMPARE(tabs->currentIndex(),0);
            QVERIFY(dialog->windowFlags().testFlag(Qt::WindowMaximizeButtonHint));
            dialog->findChild<QLineEdit *>("code_block_name")->setText("Controller");
            auto *inputs = dialog->findChild<QTableWidget *>("code_block_inputs");
            QVERIFY(inputs && inputs->rowCount() == 2);
            inputs->item(0, 0)->setText("measured");
            inputs->selectRow(0);
            dialog->findChild<QPushButton *>("move_code_input_down")->click();
            dialog->findChild<QPlainTextEdit *>("code_block_code")->setPlainText("result = measured;");
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            buttons->button(QDialogButtonBox::Ok)->click();
        });
        window.findChild<QAction *>("action_properties")->trigger();
        first_safety.stop();
        QCOMPARE(window.project().code_blocks[1].name, std::string("Controller"));
        QCOMPARE(window.project().code_blocks[1].inputs[1].name, std::string("measured"));
        QCOMPARE(window.project().code_blocks[1].inputs[1].id, input_id);
        QCOMPARE(window.project().wires[0].to, (Endpoint{sink.id, input_id}));
        window.undo();
        QCOMPARE(window.project().code_blocks[1].inputs[0].name, std::string("input"));
        window.redo();
        QCOMPARE(window.project().code_blocks[1].inputs[1].name, std::string("measured"));

        window.select_object(sink.id);
        const auto blocks_before_rejected_edit = window.project().code_blocks;
        const auto wires_before_rejected_edit = window.project().wires;
        QTimer second_safety;
        second_safety.setSingleShot(true);
        connect(&second_safety, &QTimer::timeout, [&] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
        });
        second_safety.start(3000);
        QTimer::singleShot(50, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *inputs = dialog->findChild<QTableWidget *>("code_block_inputs");
            inputs->selectRow(1);
            dialog->findChild<QPushButton *>("remove_code_input")->click();
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            buttons->button(QDialogButtonBox::Ok)->click();
            QVERIFY(dialog->findChild<QLabel *>("code_block_status")->text().contains("connected"));
            buttons->button(QDialogButtonBox::Cancel)->click();
        });
        window.findChild<QAction *>("action_properties")->trigger();
        second_safety.stop();
        QVERIFY(window.project().code_blocks == blocks_before_rejected_edit);
        QVERIFY(window.project().wires == wires_before_rejected_edit);
    }
    void code_block_library_run_record_and_reopen() {
        QTemporaryDir temp;
        EditorWindow window("en", temp.path());
        window.show();
        auto *library = window.findChild<QTreeWidget *>("library");
        QVERIFY(library);
        QTreeWidgetItem *entry = nullptr;
        for (QTreeWidgetItemIterator it(library); *it; ++it)
            if ((*it)->data(0, Qt::UserRole).toInt() == 108) entry = *it;
        QVERIFY(entry);
        QCOMPARE(entry->parent()->child(0), entry);
        for (auto *parent = entry->parent(); parent; parent = parent->parent()) parent->setExpanded(true);
        library->scrollToItem(entry);
        QTest::mouseClick(library->viewport(), Qt::LeftButton, Qt::NoModifier,
                          library->visualItemRect(entry).center());
        QTest::mouseDClick(library->viewport(), Qt::LeftButton, Qt::NoModifier,
                           library->visualItemRect(entry).center());
        const QPointF block_position(100, 100);
        QTest::mouseClick(window.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                          window.canvas()->mapFromScene(block_position));
        QCOMPARE(window.project().code_blocks.size(), size_t(1));
        const auto block = window.project().code_blocks.front().id;
        const auto output = window.project().code_blocks.front().outputs.front().id;
        QCOMPARE(window.port_position({block, output}), QPointF(160, 100));
        QGraphicsItem *block_item = nullptr;
        for (auto *item : window.canvas()->scene()->items())
            if (item->data(0).toString().toStdString() == block && item->data(1).toString() == "atom")
                block_item = item;
        QVERIFY(block_item);
        QCOMPARE(block_item->shape().boundingRect().size(), QSizeF(76, 44));
        const auto plot = window.add_plot({420, 100});
        QVERIFY(window.connect_ports({block, output}, {plot, "in1"}));
        const auto voltage = window.add_component(Kind::voltage, {40, 300});
        const auto resistor = window.add_component(Kind::resistor, {260, 300});
        const auto ground = window.add_node(true, {150, 420});
        QVERIFY(window.connect_ports({voltage, "p"}, {resistor, "p"}));
        QVERIFY(window.connect_ports({resistor, "n"}, {ground, "node"}));
        QVERIFY(window.connect_ports({voltage, "n"}, {ground, "node"}));
        const auto channel = endpoint_key({block, output});
        auto configured = window.project();
        configured.profile.stop = 5e-6;
        configured.profile.step = 1e-6;
        configured.scope_enabled = true;
        configured.scope_points = {channel};
        configured.scope_channels = {channel};
        window.set_project(configured);
        QVERIFY(window.scope());
        const auto path = temp.filePath("code-block.pds");
        QVERIFY(window.save_project(path));
        QVERIFY(window.open_project(path));
        QCOMPARE(window.project().code_blocks.size(), size_t(1));
        QCOMPARE(window.project().code_blocks.front().outputs.front().id, output);
        QCOMPARE(window.project().wires.size(), size_t(4));
        QVERIFY(std::any_of(window.project().wires.begin(), window.project().wires.end(), [&](const Wire &wire) {
            return wire.from == Endpoint{block, output} || wire.to == Endpoint{block, output};
        }));
        auto *insert = window.findChild<QAction *>("insert_component_108");
        QVERIFY(insert && insert->isEnabled());
        window.start_simulation();
        QVERIFY(window.running());
        QVERIFY(!insert->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!window.running(), 5000);
        QVERIFY(window.has_result());
        const auto recorded = std::find_if(window.result().channels.begin(), window.result().channels.end(),
                                           [&](const Channel &candidate) { return candidate.object == channel; });
        QVERIFY(recorded != window.result().channels.end());
        const auto index = size_t(recorded - window.result().channels.begin());
        QVERIFY(!window.result().samples.empty());
        QCOMPARE(window.result().samples.back().values[index], 0.0);
        window.open_plot(plot);
        QVERIFY(window.findChild<QDialog *>("plot_" + QString::fromStdString(plot)));
        configured.code_blocks.front().outputs.front().type = SignalScalarType::boolean;
        window.set_project(configured);
        bool bool_port_found = false;
        for (auto *item : window.canvas()->scene()->items())
            if (item->data(0).toString().toStdString() == block &&
                item->data(2).toString() == QString::fromStdString(output)) {
                QCOMPARE(item->data(11).toString(), QString("#17866d"));
                bool_port_found = true;
            }
        QVERIFY(bool_port_found);
    }
    void inverter_pwm_and_pi_presets_produce_typed_outputs() {
        auto run = [](int id, double time, const std::map<std::string, double> &values,
                      CProgramState *state = nullptr) {
            const auto *preset = signal_preset(id);
            if (!preset) throw std::runtime_error("Missing signal preset");
            const auto block = make_signal_preset(*preset, "test", 0, 0);
            CProgramOptions options;
            options.allow_time = true;
            options.external_variables.insert("dt");
            for (const auto &input : block.inputs) options.external_variables.insert(input.name);
            for (const auto &output : block.outputs) {
                options.external_variables.insert(output.name);
                options.writable_variables.insert(output.name);
            }
            auto inputs = values;
            inputs["dt"] = preset->period;
            return execute_c_program(compile_c_program(block.code, options), time, state, inputs).variables;
        };
        const auto high = run(123, 0, {{"ma", 0}, {"mb", 0}, {"mc", 0}});
        const auto low = run(123, 0.0005, {{"ma", 0}, {"mb", 0}, {"mc", 0}});
        for (const auto *phase : {"A", "B", "C"}) {
            QCOMPARE(high.at(std::string(phase) + "1"), 1.0);
            QCOMPARE(high.at(std::string(phase) + "2"), 0.0);
            QCOMPARE(low.at(std::string(phase) + "1"), 0.0);
            QCOMPARE(low.at(std::string(phase) + "2"), 1.0);
        }
        const auto npc = run(124, 0.0005, {{"ma", 1}, {"mb", 0}, {"mc", -1}});
        for (const auto &[phase, pattern] : {
                 std::pair{"A", std::array{1., 1., 0., 0.}},
                 std::pair{"B", std::array{0., 1., 1., 0.}},
                 std::pair{"C", std::array{0., 0., 1., 1.}}})
            for (int gate = 1; gate <= 4; ++gate)
                QCOMPARE(npc.at(std::string(phase) + std::to_string(gate)), pattern[gate - 1]);
        CProgramState state;
        const auto saturated = run(122, 0, {{"reference", 2}, {"feedback", 0}}, &state);
        const auto recovered = run(122, 0.0001, {{"reference", 0}, {"feedback", 0}}, &state);
        QCOMPARE(saturated.at("out"), 1.0);
        QCOMPARE(recovered.at("out"), 0.0);
        CProgramState pid_state;
        const auto pid_initial = run(125, 0, {{"reference", 0}, {"feedback", 0}}, &pid_state);
        const auto pid_step = run(125, 0.0001, {{"reference", 0}, {"feedback", 0.1}}, &pid_state);
        QCOMPARE(pid_initial.at("out"), 0.0);
        QVERIFY(pid_step.at("out") < -0.1 && pid_step.at("out") > -1.0);
        QTemporaryDir temp;
        EditorWindow window("en", temp.path());
        window.show();
        auto *library = window.findChild<QTreeWidget *>("library");
        QVERIFY(library);
        for (const auto [id, inputs, outputs] : {
                 std::tuple{122, size_t(2), size_t(1)},
                 std::tuple{123, size_t(3), size_t(6)},
                 std::tuple{124, size_t(3), size_t(12)},
                 std::tuple{125, size_t(2), size_t(1)}}) {
            QTreeWidgetItem *entry = nullptr;
            for (QTreeWidgetItemIterator it(library); *it; ++it)
                if ((*it)->data(0, Qt::UserRole).toInt() == id) entry = *it;
            QVERIFY(entry);
            for (auto *parent = entry->parent(); parent; parent = parent->parent()) parent->setExpanded(true);
            library->scrollToItem(entry);
            QTest::mouseClick(library->viewport(), Qt::LeftButton, Qt::NoModifier,
                              library->visualItemRect(entry).center());
            QTest::mouseDClick(library->viewport(), Qt::LeftButton, Qt::NoModifier,
                               library->visualItemRect(entry).center());
            QTest::mouseClick(window.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                              window.canvas()->mapFromScene(QPointF(100 + 200 * (id - 122), 100)));
            QCOMPARE(window.project().code_blocks.size(), size_t(id - 121));
            QCOMPARE(window.project().code_blocks.back().inputs.size(), inputs);
            QCOMPARE(window.project().code_blocks.back().outputs.size(), outputs);
        }
    }
    void two_level_pwm_preset_drives_library_inverter() {
        std::ifstream input(PDS_SOURCE_DIR "/examples/vsi-2l.pds");
        QVERIFY(input.good());
        auto project = read_project(input);
        auto gate_instance = std::find_if(project.instances.begin(), project.instances.end(),
                                          [](const Instance &item) { return item.name == "Programmable gates"; });
        QVERIFY(gate_instance != project.instances.end());
        const auto gate_id = gate_instance->id;
        const auto gate_definition_id = gate_instance->definition;
        const auto gate_definition = definition(project, gate_instance->definition);
        const auto *preset = signal_preset(123);
        QVERIFY(preset);
        auto pwm = make_signal_preset(*preset, "2L signal PWM", -560, -60);
        pwm.id = derived_uuid("example:vsi-2l-signal-pwm:modulator");
        for (auto &port : pwm.inputs)
            port.id = derived_uuid("example:vsi-2l-signal-pwm:input:" + port.name);
        for (auto &port : pwm.outputs)
            port.id = derived_uuid("example:vsi-2l-signal-pwm:output:" + port.name);
        const auto pwm_id = pwm.id;
        for (auto &wire : project.wires) {
            if (wire.from.object != gate_id) continue;
            const auto old_port = std::find_if(gate_definition.ports.begin(), gate_definition.ports.end(),
                                                [&](const PublicPort &port) { return port.id == wire.from.port; });
            QVERIFY(old_port != gate_definition.ports.end());
            const auto new_port = std::find_if(pwm.outputs.begin(), pwm.outputs.end(),
                                                [&](const CodePort &port) { return port.name == old_port->name; });
            QVERIFY(new_port != pwm.outputs.end());
            wire.from = {pwm_id, new_port->id};
        }
        project.instances.erase(gate_instance);
        std::erase_if(project.definitions, [&](const Definition &item) { return item.id == gate_definition_id; });
        const auto *sine_preset = signal_preset(112);
        QVERIFY(sine_preset);
        for (int phase = 0; phase < 3; ++phase) {
            auto sine = make_signal_preset(*sine_preset, std::string("Modulation ") + char('A' + phase),
                                           -820, -220 + phase * 140);
            sine.id = derived_uuid("example:vsi-2l-signal-pwm:sine:" + std::to_string(phase));
            sine.outputs.front().id = derived_uuid("example:vsi-2l-signal-pwm:sine-output:" + std::to_string(phase));
            sine.period = 10e-6;
            sine.code = "out = 0.7 * sin(2 * PI * 50 * t + " + std::to_string(phase * -2.0 * 3.141592653589793 / 3.0) + ");";
            project.wires.push_back({derived_uuid("example:vsi-2l-signal-pwm:signal-wire:" + std::to_string(phase)),
                                     {sine.id, sine.outputs.front().id},
                                     {pwm_id, pwm.inputs[phase].id}});
            project.code_blocks.push_back(std::move(sine));
        }
        project.code_blocks.push_back(std::move(pwm));
        project.id = derived_uuid("example:vsi-2l-signal-pwm:project");
        project.name = "2L VSI with signal PWM";
        project.profile.stop = 0.006;
        const auto ir = compile(project);
        QCOMPARE(ir.signal_gates.size(), size_t(6));
        const auto result = execute(ir);
        QVERIFY(!result.cancelled && result.accepted_steps >= 600);
        bool positive = false, negative = false;
        const auto phase_voltage = std::find_if(result.channels.begin(), result.channels.end(),
                                                [](const Channel &channel) { return channel.name == "u:UA"; });
        QVERIFY(phase_voltage != result.channels.end());
        const auto index = size_t(phase_voltage - result.channels.begin());
        for (const auto &sample : result.samples) {
            positive |= sample.values[index] > 100;
            negative |= sample.values[index] < -100;
        }
        QVERIFY(positive && negative);
        const auto example_path = qEnvironmentVariable("PDS_PWM_EXAMPLE_PATH");
        if (!example_path.isEmpty()) {
            std::ofstream output(example_path.toStdString());
            QVERIFY(output.good());
            write_project(project, output);
            QVERIFY(output.good());
            output.close();
            if (const auto screenshot = qEnvironmentVariable("PDS_PWM_EXAMPLE_SCREENSHOT"); !screenshot.isEmpty()) {
                QTemporaryDir temp;
                EditorWindow window("en", temp.path());
                window.resize(1400, 900);
                window.show();
                QVERIFY(window.open_project(example_path));
                QTest::qWait(30);
                QVERIFY(window.grab().save(screenshot));
            }
        }
    }
    void signal_presets_are_editable_code_blocks() {
        QTemporaryDir temp;
        EditorWindow window("en", temp.path());
        window.show();
        auto *library = window.findChild<QTreeWidget *>("library");
        QVERIFY(library);
        struct Preset { int id; const char *code; };
        const std::array<Preset, 4> presets{{
            {109, "out = 1;"},
            {110, "out = t >= 5e-3 ? 1 : 0;"},
            {111, "out = t < 10e-3 ? t / 10e-3 : 1;"},
            {112, "out = sin(2 * PI * 50 * t);"},
        }};
        std::vector<std::string> blocks, outputs;
        std::vector<std::vector<IconPrimitive>> icons;
        std::vector<QImage> library_icons;
        for (size_t index = 0; index < presets.size(); ++index) {
            QTreeWidgetItem *entry = nullptr;
            for (QTreeWidgetItemIterator it(library); *it; ++it)
                if ((*it)->data(0, Qt::UserRole).toInt() == presets[index].id)
                    entry = *it;
            QVERIFY(entry);
            library_icons.push_back(entry->icon(0).pixmap(32,32).toImage());
            for (auto *parent = entry->parent(); parent; parent = parent->parent()) parent->setExpanded(true);
            library->scrollToItem(entry);
            QTest::mouseClick(library->viewport(), Qt::LeftButton, Qt::NoModifier,
                              library->visualItemRect(entry).center());
            QTest::mouseDClick(library->viewport(), Qt::LeftButton, Qt::NoModifier,
                               library->visualItemRect(entry).center());
            QTest::mouseClick(window.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                              window.canvas()->mapFromScene(QPointF(100 + 220 * index, 100)));
            QCOMPARE(window.project().code_blocks.size(), index + 1);
            const auto &block = window.project().code_blocks.back();
            QCOMPARE(block.code, std::string(presets[index].code));
            QCOMPARE(block.period, 100e-6);
            QCOMPARE(block.outputs.size(), size_t(1));
            QCOMPARE(block.outputs.front().type, SignalScalarType::real);
            QVERIFY(!block.icon.empty());
            blocks.push_back(block.id);
            outputs.push_back(block.outputs.front().id);
            icons.push_back(block.icon);
        }
        for (size_t left = 0; left < icons.size(); ++left)
            for (size_t right = left + 1; right < icons.size(); ++right)
                QVERIFY(icons[left] != icons[right]);
        for (size_t left = 0; left < library_icons.size(); ++left)
            for (size_t right = left + 1; right < library_icons.size(); ++right)
                QVERIFY(library_icons[left] != library_icons[right]);

        window.select_object(blocks.back());
        QTimer edit_safety;
        edit_safety.setSingleShot(true);
        connect(&edit_safety, &QTimer::timeout, [] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                dialog->reject();
        });
        edit_safety.start(2000);
        QTimer::singleShot(50, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            QVERIFY(dialog->findChild<QWidget *>("code_icon_canvas"));
            dialog->findChild<QLineEdit *>("code_block_name")->setText("Reference sine");
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            if (buttons) buttons->button(QDialogButtonBox::Ok)->click();
        });
        window.findChild<QAction *>("action_properties")->trigger();
        edit_safety.stop();
        QCOMPARE(window.project().code_blocks.back().name, std::string("Reference sine"));
        window.undo();
        QCOMPARE(window.project().code_blocks.back().name, std::string("Sine"));
        window.redo();
        QCOMPARE(window.project().code_blocks.back().name, std::string("Reference sine"));

        const auto voltage = window.add_component(Kind::voltage, {80, 320});
        const auto resistor = window.add_component(Kind::resistor, {280, 320});
        const auto ground = window.add_node(true, {180, 440});
        QVERIFY(window.connect_ports({voltage, "p"}, {resistor, "p"}));
        QVERIFY(window.connect_ports({resistor, "n"}, {ground, "node"}));
        QVERIFY(window.connect_ports({voltage, "n"}, {ground, "node"}));

        auto configured = window.project();
        configured.profile.stop = 10.1e-3;
        configured.profile.step = 100e-6;
        configured.scope_enabled = true;
        for (size_t index = 0; index < blocks.size(); ++index) {
            const auto channel = endpoint_key({blocks[index], outputs[index]});
            configured.scope_points.push_back(channel);
            configured.scope_channels.push_back(channel);
        }
        window.set_project(configured);
        QVERIFY(window.scope());
        const auto path = temp.filePath("signal-presets.pds");
        QVERIFY(window.save_project(path));
        QVERIFY(window.open_project(path));
        QCOMPARE(window.project().code_blocks.size(), presets.size());
        for (size_t index = 0; index < presets.size(); ++index) {
            QCOMPARE(window.project().code_blocks[index].code, std::string(presets[index].code));
            QCOMPARE(window.project().code_blocks[index].period, 100e-6);
        }

        window.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!window.running(), 5000);
        QVERIFY(window.has_result());
        std::array<size_t, 4> channels{};
        for (size_t index = 0; index < channels.size(); ++index) {
            const auto key = endpoint_key({blocks[index], outputs[index]});
            const auto channel = std::find_if(window.result().channels.begin(), window.result().channels.end(),
                                              [&](const Channel &candidate) { return candidate.object == key; });
            QVERIFY(channel != window.result().channels.end());
            channels[index] = size_t(channel - window.result().channels.begin());
        }
        auto sample_at = [&](double time) -> const Sample * {
            const auto sample = std::find_if(window.result().samples.begin(), window.result().samples.end(),
                                             [&](const Sample &candidate) { return std::abs(candidate.time - time) < 1e-12; });
            return sample == window.result().samples.end() ? nullptr : &*sample;
        };
        const auto *before_step = sample_at(4.9e-3);
        QVERIFY(before_step);
        QCOMPARE(before_step->values[channels[0]], 1.0);
        QCOMPARE(before_step->values[channels[1]], 0.0);
        QVERIFY(std::abs(before_step->values[channels[2]] - .49) < 1e-12);
        QVERIFY(std::abs(before_step->values[channels[3]] - std::sin(2 * std::acos(-1.0) * 50 * 4.9e-3)) < 1e-12);
        const auto *step = sample_at(5.1e-3);
        QVERIFY(step);
        QCOMPARE(step->values[channels[1]], 1.0);
        QVERIFY(std::abs(step->values[channels[2]] - .51) < 1e-12);
        QVERIFY(std::abs(step->values[channels[3]] - std::sin(2 * std::acos(-1.0) * 50 * 5.1e-3)) < 1e-12);
        const auto *ramp_end = sample_at(10.1e-3);
        QVERIFY(ramp_end);
        QVERIFY(std::abs(ramp_end->values[channels[2]] - 1) < 1e-12);
        QVERIFY(std::abs(ramp_end->values[channels[3]] - std::sin(2 * std::acos(-1.0) * 50 * 10.1e-3)) < 1e-12);
    }
    void signal_operator_presets_use_editable_code_block_factory() {
        QTemporaryDir temp;
        EditorWindow window("en", temp.path());
        window.show();
        auto *library = window.findChild<QTreeWidget *>("library");
        QVERIFY(library);
        struct Preset {
            int id;
            const char *code;
            std::vector<std::string> input_names;
            SignalScalarType input_type;
            SignalScalarType output_type;
        };
        const std::vector<Preset> presets{
            {113, "out = a + b;", {"a", "b"}, SignalScalarType::real, SignalScalarType::real},
            {114, "out = clamp(in, -1, 1);", {"in"}, SignalScalarType::real, SignalScalarType::real},
            {115, "out = a >= b;", {"a", "b"}, SignalScalarType::real, SignalScalarType::boolean},
            {116, "out = a && b;", {"a", "b"}, SignalScalarType::boolean, SignalScalarType::boolean},
        };
        for (size_t index = 0; index < presets.size(); ++index) {
            QTreeWidgetItem *entry = nullptr;
            for (QTreeWidgetItemIterator it(library); *it; ++it)
                if ((*it)->data(0, Qt::UserRole).toInt() == presets[index].id)
                    entry = *it;
            QVERIFY(entry);
            for (auto *parent = entry->parent(); parent; parent = parent->parent()) parent->setExpanded(true);
            library->scrollToItem(entry);
            QTest::mouseClick(library->viewport(), Qt::LeftButton, Qt::NoModifier,
                              library->visualItemRect(entry).center());
            QTest::mouseDClick(library->viewport(), Qt::LeftButton, Qt::NoModifier,
                               library->visualItemRect(entry).center());
            QTest::mouseClick(window.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                              window.canvas()->mapFromScene(QPointF(120 + 220 * index, 100)));
            const auto &block = window.project().code_blocks.back();
            QCOMPARE(block.code, std::string(presets[index].code));
            QCOMPARE(block.period, 100e-6);
            QCOMPARE(block.inputs.size(), presets[index].input_names.size());
            QCOMPARE(block.outputs.size(), size_t(1));
            for (size_t input = 0; input < block.inputs.size(); ++input) {
                QCOMPARE(block.inputs[input].name, presets[index].input_names[input]);
                QCOMPARE(block.inputs[input].type, presets[index].input_type);
                QCOMPARE(block.inputs[input].unit, std::string());
                QVERIFY(!block.inputs[input].id.empty());
            }
            QCOMPARE(block.outputs[0].name, std::string("out"));
            QCOMPARE(block.outputs[0].type, presets[index].output_type);
            QCOMPARE(block.outputs[0].unit, std::string());
            QVERIFY(!block.outputs[0].id.empty());
        }
        window.undo();
        QCOMPARE(window.project().code_blocks.size(), presets.size() - 1);
        window.redo();
        QCOMPARE(window.project().code_blocks.size(), presets.size());
        const auto path = temp.filePath("signal-operators.pds");
        QVERIFY(window.save_project(path));
        QVERIFY(window.open_project(path));
        QCOMPARE(window.project().code_blocks.size(), presets.size());
        for (size_t index = 0; index < presets.size(); ++index)
            QCOMPARE(window.project().code_blocks[index].code, std::string(presets[index].code));
    }
    void carrier_and_pwm_comparator_presets_use_editable_code_block_factory() {
        QTemporaryDir temp;
        EditorWindow window("en", temp.path());
        window.show();
        auto *library = window.findChild<QTreeWidget *>("library");
        QVERIFY(library);
        auto place = [&](int id, QPointF point) {
            QTreeWidgetItem *entry = nullptr;
            for (QTreeWidgetItemIterator it(library); *it; ++it)
                if ((*it)->data(0, Qt::UserRole).toInt() == id)
                    entry = *it;
            QVERIFY(entry);
            for (auto *parent = entry->parent(); parent; parent = parent->parent())
                parent->setExpanded(true);
            library->scrollToItem(entry);
            QTest::mouseClick(library->viewport(), Qt::LeftButton, Qt::NoModifier,
                              library->visualItemRect(entry).center());
            QTest::mouseDClick(library->viewport(), Qt::LeftButton, Qt::NoModifier,
                               library->visualItemRect(entry).center());
            QTest::mouseClick(window.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                              window.canvas()->mapFromScene(point));
        };
        place(119, {120, 100});
        QCOMPARE(window.project().code_blocks.size(), size_t(1));
        const auto &carrier = window.project().code_blocks[0];
        QCOMPARE(carrier.name, std::string("Carrier generator"));
        QCOMPARE(carrier.code, std::string("double phase = t * 1000 - floor(t * 1000);\nout = 1 - 4 * abs(phase - 0.5);"));
        QCOMPARE(carrier.period, 100e-6);
        QCOMPARE(carrier.inputs.size(), size_t(0));
        QCOMPARE(carrier.outputs.size(), size_t(1));
        QCOMPARE(carrier.outputs[0].name, std::string("out"));
        QCOMPARE(carrier.outputs[0].type, SignalScalarType::real);
        QVERIFY(!carrier.outputs[0].id.empty());
        const auto carrier_code = carrier.code;

        place(120, {340, 100});
        QCOMPARE(window.project().code_blocks.size(), size_t(2));
        const auto &comparator = window.project().code_blocks[1];
        QCOMPARE(comparator.name, std::string("PWM comparator"));
        QCOMPARE(comparator.code, std::string("out = reference >= carrier;"));
        QCOMPARE(comparator.period, 100e-6);
        QCOMPARE(comparator.inputs.size(), size_t(2));
        QCOMPARE(comparator.inputs[0].name, std::string("reference"));
        QCOMPARE(comparator.inputs[1].name, std::string("carrier"));
        QCOMPARE(comparator.inputs[0].type, SignalScalarType::real);
        QCOMPARE(comparator.inputs[1].type, SignalScalarType::real);
        QCOMPARE(comparator.outputs.size(), size_t(1));
        QCOMPARE(comparator.outputs[0].name, std::string("out"));
        QCOMPARE(comparator.outputs[0].type, SignalScalarType::boolean);
        QVERIFY(!comparator.inputs[0].id.empty());
        QVERIFY(!comparator.inputs[1].id.empty());
        QVERIFY(!comparator.outputs[0].id.empty());
        const auto comparator_code = comparator.code;

        window.undo();
        QCOMPARE(window.project().code_blocks.size(), size_t(1));
        window.redo();
        QCOMPARE(window.project().code_blocks.size(), size_t(2));
        const auto path = temp.filePath("carrier-pwm-presets.pds");
        QVERIFY(window.save_project(path));
        QVERIFY(window.open_project(path));
        QCOMPARE(window.project().code_blocks.size(), size_t(2));
        QCOMPARE(window.project().code_blocks[0].code, carrier_code);
        QCOMPARE(window.project().code_blocks[1].code, comparator_code);
        QCOMPARE(window.project().code_blocks[1].inputs[0].name, std::string("reference"));
        QCOMPARE(window.project().code_blocks[1].inputs[1].name, std::string("carrier"));
        QCOMPARE(window.project().code_blocks[1].outputs[0].type, SignalScalarType::boolean);
    }
    void signal_state_presets_are_causal_and_snapshot_safe() {
        const auto *integrator_preset = signal_preset(117);
        const auto *delay_preset = signal_preset(118);
        const auto *hold_preset = signal_preset(121);
        QVERIFY(integrator_preset);
        QVERIFY(delay_preset);
        QVERIFY(hold_preset);

        CodeBlock source;
        source.id = new_uuid();
        source.name = "Source";
        source.period = 100e-6;
        source.code = "out = 2;";
        source.outputs = {{new_uuid(), "out", "", SignalScalarType::real, 0}};
        auto integrator = make_signal_preset(*integrator_preset, "Discrete integrator", 180, 100);
        auto delay = make_signal_preset(*delay_preset, "Delay 1 tick", 360, 100);
        auto hold = make_signal_preset(*hold_preset, "Sample and hold", 540, 100);
        CodeBlock trigger;
        trigger.id = new_uuid();
        trigger.name = "Sample pulse";
        trigger.period = 100e-6;
        trigger.code = "out = t >= 100e-6 && t < 200e-6;";
        trigger.outputs = {{new_uuid(), "out", "", SignalScalarType::boolean, 0}};

        Project project;
        project.id = new_uuid();
        project.wired = true;
        project.profile.step = 100e-6;
        project.profile.stop = 400e-6;
        const auto ground = new_uuid();
        const auto supply_node = new_uuid();
        project.nodes = {{ground, "GND", true}, {supply_node, "supply"}};
        Component supply;
        supply.id = new_uuid();
        supply.name = "V1";
        supply.kind = Kind::voltage;
        supply.value = 1;
        project.components = {supply};
        project.code_blocks = {source, integrator, delay, trigger, hold};
        project.wires = {
            {new_uuid(), {supply.id, "p"}, {supply_node, "node"}},
            {new_uuid(), {supply.id, "n"}, {ground, "node"}},
            {new_uuid(), {source.id, source.outputs[0].id}, {integrator.id, integrator.inputs[0].id}},
            {new_uuid(), {source.id, source.outputs[0].id}, {delay.id, delay.inputs[0].id}},
            {new_uuid(), {source.id, source.outputs[0].id}, {hold.id, hold.inputs[0].id}},
            {new_uuid(), {trigger.id, trigger.outputs[0].id}, {hold.id, hold.inputs[1].id}},
        };
        SimulationIR ir;
        try {
            ir = compile(project);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
        QCOMPARE(ir.signal.tasks.size(), size_t(5));
        Result complete;
        try {
            complete = execute(ir);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
        const auto integrator_key = endpoint_key({integrator.id, integrator.outputs[0].id});
        const auto delay_key = endpoint_key({delay.id, delay.outputs[0].id});
        const auto hold_key = endpoint_key({hold.id, hold.outputs[0].id});
        const auto integrator_found = std::find_if(complete.channels.begin(), complete.channels.end(),
                                                   [&](const Channel &channel) { return channel.object == integrator_key; });
        const auto delay_found = std::find_if(complete.channels.begin(), complete.channels.end(),
                                              [&](const Channel &channel) { return channel.object == delay_key; });
        const auto hold_found = std::find_if(complete.channels.begin(), complete.channels.end(),
                                             [&](const Channel &channel) { return channel.object == hold_key; });
        QVERIFY(integrator_found != complete.channels.end());
        QVERIFY(delay_found != complete.channels.end());
        QVERIFY(hold_found != complete.channels.end());
        const auto integrator_channel = size_t(integrator_found - complete.channels.begin());
        const auto delay_channel = size_t(delay_found - complete.channels.begin());
        const auto hold_channel = size_t(hold_found - complete.channels.begin());
        auto sample_at = [&](double time) -> const Sample * {
            const auto found = std::find_if(complete.samples.begin(), complete.samples.end(),
                                            [&](const Sample &sample) { return std::abs(sample.time - time) < 1e-15; });
            return found == complete.samples.end() ? nullptr : &*found;
        };
        const auto *initial = sample_at(0);
        const auto *first = sample_at(100e-6);
        const auto *second = sample_at(200e-6);
        const auto *third = sample_at(300e-6);
        QVERIFY(initial);
        QVERIFY(first);
        QVERIFY(second);
        QVERIFY(third);
        QCOMPARE(initial->values[integrator_channel], 0.0);
        QCOMPARE(initial->values[delay_channel], 0.0);
        QCOMPARE(first->values[integrator_channel], 200e-6);
        QCOMPARE(first->values[delay_channel], 0.0);
        QCOMPARE(second->values[integrator_channel], 400e-6);
        QCOMPARE(second->values[delay_channel], 2.0);
        QCOMPARE(initial->values[hold_channel], 0.0);
        QCOMPARE(first->values[hold_channel], 0.0);
        QCOMPARE(second->values[hold_channel], 2.0);
        QCOMPARE(third->values[hold_channel], 2.0);

        ExecutionOptions snapshot_options;
        snapshot_options.capture_snapshot = true;
        snapshot_options.max_steps = 2;
        const auto partial = execute(ir, nullptr, nullptr, nullptr, nullptr, {}, &snapshot_options);
        QVERIFY(partial.snapshot);
        QCOMPARE(partial.last_time, 200e-6);
        QCOMPARE(partial.snapshot->signal_tasks.at(integrator.id).next_tick, std::uint64_t(3));
        QCOMPARE(partial.snapshot->signal_tasks.at(delay.id).next_tick, std::uint64_t(3));
        QCOMPARE(partial.snapshot->signal_tasks.at(integrator.id).outputs.at("out"), 400e-6);
        QCOMPARE(partial.snapshot->signal_tasks.at(delay.id).outputs.at("out"), 2.0);
        QCOMPARE(partial.snapshot->signal_tasks.at(hold.id).outputs.at("out"), 2.0);
        QVERIFY(!partial.snapshot->signal_tasks.at(hold.id).program_state.static_values.empty());
        std::ostringstream saved;
        write_snapshot(*partial.snapshot, saved);
        std::istringstream input(saved.str());
        const auto restored = read_snapshot(input);
        QCOMPARE(restored, *partial.snapshot);
        ExecutionOptions continuation;
        continuation.resume = &restored;
        const auto resumed = execute(ir, nullptr, nullptr, nullptr, nullptr, {}, &continuation);
        QCOMPARE(resumed.samples.size(), complete.samples.size() - partial.accepted_steps);
        for (size_t index = 0; index < resumed.samples.size(); ++index) {
            QCOMPARE(resumed.samples[index].time, complete.samples[partial.accepted_steps + index].time);
            QCOMPARE(resumed.samples[index].values, complete.samples[partial.accepted_steps + index].values);
            QCOMPARE(resumed.samples[index].gates, complete.samples[partial.accepted_steps + index].gates);
        }

        auto disconnected = project;
        std::erase_if(disconnected.wires, [&](const Wire &wire) {
            return wire.to == Endpoint{integrator.id, integrator.inputs[0].id};
        });
        try {
            (void)compile(disconnected);
            QFAIL("Unconnected preset input compiled");
        } catch (const Diagnostic &diagnostic) {
            QCOMPARE(diagnostic.code, std::string("missing_signal_source"));
            QCOMPARE(diagnostic.object, integrator.id);
        }

        QTemporaryDir temp;
        EditorWindow window("en", temp.path());
        window.show();
        auto *library = window.findChild<QTreeWidget *>("library");
        QVERIFY(library);
        for (const auto *preset : {integrator_preset, delay_preset}) {
            QTreeWidgetItem *entry = nullptr;
            for (QTreeWidgetItemIterator it(library); *it; ++it)
                if ((*it)->data(0, Qt::UserRole).toInt() == preset->placement_id)
                    entry = *it;
            QVERIFY(entry);
            for (auto *parent = entry->parent(); parent; parent = parent->parent()) parent->setExpanded(true);
            library->scrollToItem(entry);
            QTest::mouseClick(library->viewport(), Qt::LeftButton, Qt::NoModifier,
                              library->visualItemRect(entry).center());
            QTest::mouseDClick(library->viewport(), Qt::LeftButton, Qt::NoModifier,
                               library->visualItemRect(entry).center());
            QTest::mouseClick(window.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                              window.canvas()->mapFromScene(QPointF(140 + 220 * window.project().code_blocks.size(), 100)));
            const auto &block = window.project().code_blocks.back();
            QCOMPARE(block.code, std::string(preset->code));
            QCOMPARE(block.period, 100e-6);
            QCOMPARE(block.inputs.size(), size_t(1));
            QCOMPARE(block.outputs.size(), size_t(1));
            QCOMPARE(block.inputs[0].name, std::string("in"));
            QCOMPARE(block.outputs[0].name, std::string("out"));
            QCOMPARE(block.inputs[0].type, SignalScalarType::real);
            QCOMPARE(block.outputs[0].type, SignalScalarType::real);
            QVERIFY(!block.inputs[0].id.empty());
            QVERIFY(!block.outputs[0].id.empty());
        }
        window.undo();
        QCOMPARE(window.project().code_blocks.size(), size_t(1));
        window.redo();
        QCOMPARE(window.project().code_blocks.size(), size_t(2));
        const auto path = temp.filePath("signal-state-presets.pds");
        QVERIFY(window.save_project(path));
        QVERIFY(window.open_project(path));
        QCOMPARE(window.project().code_blocks.size(), size_t(2));
        QCOMPARE(window.project().code_blocks[0].code, std::string(integrator_preset->code));
        QCOMPARE(window.project().code_blocks[1].code, std::string(delay_preset->code));
    }
    void sample_hold_preset_has_typed_ports_and_survives_roundtrip() {
        const auto *preset = signal_preset(121);
        QVERIFY(preset);
        QTemporaryDir temp;
        EditorWindow window("en", temp.path());
        window.show();
        auto *library = window.findChild<QTreeWidget *>("library");
        QVERIFY(library);
        QTreeWidgetItem *entry = nullptr;
        for (QTreeWidgetItemIterator it(library); *it; ++it)
            if ((*it)->data(0, Qt::UserRole).toInt() == 121)
                entry = *it;
        QVERIFY(entry);
        for (auto *parent = entry->parent(); parent; parent = parent->parent())
            parent->setExpanded(true);
        library->scrollToItem(entry);
        QTest::mouseClick(library->viewport(), Qt::LeftButton, Qt::NoModifier,
                          library->visualItemRect(entry).center());
        QTest::mouseDClick(library->viewport(), Qt::LeftButton, Qt::NoModifier,
                           library->visualItemRect(entry).center());
        QTest::mouseClick(window.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                          window.canvas()->mapFromScene(QPointF(180, 100)));
        QCOMPARE(window.project().code_blocks.size(), size_t(1));
        const auto &block = window.project().code_blocks.front();
        QCOMPARE(block.name, std::string("Sample and hold"));
        QCOMPARE(block.code, std::string(preset->code));
        QCOMPARE(block.period, 100e-6);
        QCOMPARE(block.inputs.size(), size_t(2));
        QCOMPARE(block.inputs[0].name, std::string("in"));
        QCOMPARE(block.inputs[0].type, SignalScalarType::real);
        QCOMPARE(block.inputs[1].name, std::string("sample"));
        QCOMPARE(block.inputs[1].type, SignalScalarType::boolean);
        QCOMPARE(block.outputs.size(), size_t(1));
        QCOMPARE(block.outputs[0].type, SignalScalarType::real);
        const auto input_ids = std::pair{block.inputs[0].id, block.inputs[1].id};
        window.undo();
        QCOMPARE(window.project().code_blocks.size(), size_t(0));
        window.redo();
        QCOMPARE(window.project().code_blocks.size(), size_t(1));
        const auto path = temp.filePath("sample-hold.pds");
        QVERIFY(window.save_project(path));
        QVERIFY(window.open_project(path));
        QCOMPARE(window.project().code_blocks.size(), size_t(1));
        QCOMPARE(window.project().code_blocks[0].inputs[0].id, input_ids.first);
        QCOMPARE(window.project().code_blocks[0].inputs[1].id, input_ids.second);
        QCOMPARE(window.project().code_blocks[0].code, std::string(preset->code));
    }
};
int main(int argc, char **argv) { return run_qt_test<DesktopTests>(argc, argv); }
#include "desktop_tests.moc"
