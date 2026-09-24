#include "apps/desktop/editor.hpp"
#include "apps/desktop/code_editor.hpp"
#include "apps/desktop/routing.hpp"
#include "apps/desktop/theme.hpp"
#include "core/editor/properties.hpp"
#include "core/model/hierarchy.hpp"
#include "formats/project/project.hpp"
#include "benchmarks/metrics.hpp"
#include "tests/qt_test_main.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QBuffer>
#include <QComboBox>
#include <QCompleter>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
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
#include <QTreeWidgetItemIterator>
#include <QToolTip>
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
    void embedded_definition_image_refreshes_after_edit_and_undo() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        ready(w);
        const auto resistor = w.project().components[1].id;
        w.select_object(resistor);
        const auto grouped = w.group_selection("Image cell");
        QVERIFY(!grouped.empty());
        QImage source(12, 8, QImage::Format_ARGB32_Premultiplied);
        source.fill(QColor("#d34f62"));
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(source.save(&buffer, "PNG"));
        auto project = w.root_project();
        const auto definition_id = project.instances.front().definition;
        auto body = std::find_if(project.definitions.begin(), project.definitions.end(),
                                 [&](const Definition &candidate) { return candidate.id == definition_id; });
        QVERIFY(body != project.definitions.end());
        body->appearance.image_png = png.toBase64().toStdString();
        w.set_project(std::move(project));
        auto center_color = [&] {
            auto *block = item(w, grouped);
            if (!block) return QColor{};
            QImage canvas(180, 160, QImage::Format_ARGB32_Premultiplied);
            canvas.fill(Qt::transparent);
            QPainter painter(&canvas);
            painter.translate(90, 80);
            QStyleOptionGraphicsItem options;
            block->paint(&painter, &options, nullptr);
            painter.end();
            return canvas.pixelColor(90, 80);
        };
        QCOMPARE(center_color(), QColor("#d34f62"));
        w.select_object(grouped);
        bool cleared = false;
        QTimer::singleShot(20, &w, [&] {
            auto *dialog = w.findChild<QDialog *>("public_interface_dialog");
            if (!dialog) return;
            dialog->findChild<QPushButton *>("public_image_clear")->click();
            cleared = true;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        QTimer::singleShot(2000, &w, [&] {
            if (auto *dialog = w.findChild<QDialog *>("public_interface_dialog")) dialog->reject();
        });
        w.findChild<QAction *>("public_interface")->trigger();
        QVERIFY(cleared);
        QVERIFY(definition(w.root_project(), definition_id).appearance.image_png.empty());
        QVERIFY(center_color() != QColor("#d34f62"));
        w.undo();
        QCOMPARE(center_color(), QColor("#d34f62"));
        w.redo();
        QVERIFY(center_color() != QColor("#d34f62"));
    }
    void three_phase_thyristor_example_opens_runs_and_reloads() {
        QTemporaryDir dir;
        EditorWindow w("ru", dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/thyristor-bridge-3p.pds"));
        ready(w);
        QCOMPARE(w.project().instances.size(), size_t(2));
        const auto bridge = w.project().instances.front().id;
        const auto &body = definition(w.root_project(), w.project().instances.front().definition);
        QCOMPARE(body.components.size(), size_t(6));
        QCOMPARE(body.ports.size(), size_t(11));
        QCOMPARE(definition(w.root_project(), w.project().instances.back().definition).patterns.size(), size_t(6));
        w.canvas()->fitInView(w.canvas()->scene()->itemsBoundingRect().adjusted(-60, -60, 60, 60),
                              Qt::KeepAspectRatio);
        if (const auto screenshot = qEnvironmentVariable("PDS_THYRISTOR_3P_SCREENSHOT");
            !screenshot.isEmpty())
            QVERIFY(w.grab().save(screenshot + ".png"));
        w.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 5000);
        QVERIFY(w.has_result());
        QVERIFY(!w.result().samples.empty());
        const auto saved = encoded(w.root_project());
        const auto path = dir.filePath("thyristor-bridge-3p.pds");
        QVERIFY(w.save_project(path));
        w.open_subcircuit(bridge);
        QCOMPARE(w.project().components.size(), size_t(6));
        if (const auto screenshot = qEnvironmentVariable("PDS_THYRISTOR_3P_SCREENSHOT");
            !screenshot.isEmpty())
            QVERIFY(w.grab().save(screenshot + "-inside.png"));
        QVERIFY(w.open_project(path));
        QCOMPARE(encoded(w.root_project()), saved);
    }
    void single_phase_thyristor_example_opens_runs_and_reloads() {
        QTemporaryDir dir;
        EditorWindow w("ru", dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/thyristor-bridge-1p.pds"));
        ready(w);
        QCOMPARE(w.project().instances.size(), size_t(1));
        QCOMPARE(w.project().patterns.size(), size_t(2));
        const auto bridge = w.project().instances.front().id;
        const auto &body = definition(w.root_project(), w.project().instances.front().definition);
        QCOMPARE(body.components.size(), size_t(4));
        QCOMPARE(body.ports.size(), size_t(8));
        w.canvas()->fitInView(w.canvas()->scene()->itemsBoundingRect().adjusted(-60, -60, 60, 60),
                              Qt::KeepAspectRatio);
        if (const auto screenshot = qEnvironmentVariable("PDS_THYRISTOR_1P_SCREENSHOT");
            !screenshot.isEmpty())
            QVERIFY(w.grab().save(screenshot + ".png"));
        w.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 5000);
        QVERIFY(w.has_result());
        QVERIFY(!w.result().samples.empty());
        const auto saved = encoded(w.root_project());
        const auto path = dir.filePath("thyristor-bridge-1p.pds");
        QVERIFY(w.save_project(path));
        w.open_subcircuit(bridge);
        QCOMPARE(w.project().components.size(), size_t(4));
        if (const auto screenshot = qEnvironmentVariable("PDS_THYRISTOR_1P_SCREENSHOT");
            !screenshot.isEmpty())
            QVERIFY(w.grab().save(screenshot + "-inside.png"));
        QVERIFY(w.open_project(path));
        QCOMPARE(encoded(w.root_project()), saved);
    }
    void locked_hierarchy_navigation_keeps_definition_button_embedded() {
        QTemporaryDir dir;
        EditorWindow w("ru", dir.path());
        QVERIFY(w.open_project(PDS_SOURCE_DIR "/library/converters/ac-voltage-controller-3p.pds"));
        auto project = w.root_project();
        project.instances.front().locked = true;
        project.definitions.front().instances[1].locked = true;
        w.set_project(project);
        ready(w);
        const auto outer = project.instances.front().id;
        w.open_subcircuit(outer);
        auto *button = w.findChild<QToolButton *>("edit_definition_button");
        QVERIFY(button);
        QVERIFY(!button->isWindow());
        QCOMPARE(button->parentWidget(), w.findChild<QWidget *>("hierarchy_breadcrumbs"));
        QVERIFY(button->isVisible());
        struct TopLevelGuard : QObject {
            bool detached = false;
            bool eventFilter(QObject *object, QEvent *event) override {
                if (event->type() == QEvent::ParentChange &&
                    static_cast<QWidget *>(object)->isWindow())
                    detached = true;
                return false;
            }
        } guard;
        button->installEventFilter(&guard);
        const auto inner = w.project().instances.at(1).id;
        w.open_subcircuit(inner);
        QVERIFY(!guard.detached);
        QVERIFY(!button->isWindow());
        QTest::qWait(100);
        w.navigate_hierarchy({});
        QVERIFY(!guard.detached);
        QVERIFY(!button->isWindow());
        QVERIFY(!button->isVisible());
        // The user's hang happened on a native double-click into this locked
        // three-phase library definition, not only on the direct API path.
        const std::vector<std::string> expected{outer};
        for (int attempt = 0; attempt < 2; ++attempt) {
            auto *outer_item = item(w, outer);
            QVERIFY(outer_item);
            QTest::mouseDClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                               w.canvas()->mapFromScene(outer_item->scenePos()));
            QTRY_VERIFY_WITH_TIMEOUT(w.hierarchy_path() == expected, 1000);
            QVERIFY(!guard.detached);
            QVERIFY(!button->isWindow());
            QCOMPARE(button->parentWidget(), w.findChild<QWidget *>("hierarchy_breadcrumbs"));
            QTest::qWait(100);
            QVERIFY(button->isVisible());
            w.navigate_hierarchy({});
            QVERIFY(!guard.detached);
            QVERIFY(!button->isWindow());
            QVERIFY(!button->isVisible());
        }
    }
    void results_panel_keeps_user_height_across_tabs() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        ready(w);
        auto *dock = w.findChild<QDockWidget *>("results");
        auto *tabs = w.findChild<QTabWidget *>("results_tabs");
        QVERIFY(dock && tabs);
        w.resizeDocks({dock}, {120}, Qt::Vertical);
        QTest::qWait(30);
        const int chosen_height = dock->height();
        QVERIFY2(chosen_height <= 140, qPrintable(QString("height=%1 minimum=%2 hint=%3")
                     .arg(chosen_height).arg(dock->minimumHeight()).arg(dock->minimumSizeHint().height())));
        tabs->setCurrentIndex(1);
        QTest::qWait(30);
        QVERIFY(std::abs(dock->height() - chosen_height) <= 2);
        tabs->setCurrentIndex(0);
        QTest::qWait(30);
        QVERIFY(std::abs(dock->height() - chosen_height) <= 2);
        w.findChild<QCheckBox *>("scope_enable")->setChecked(true);
        QTest::qWait(30);
        w.resizeDocks({dock}, {120}, Qt::Vertical);
        QTest::qWait(30);
        const int recording_height = dock->height();
        QVERIFY2(recording_height <= 140, qPrintable(QString("recording height=%1 minimum=%2 hint=%3")
                     .arg(recording_height).arg(dock->minimumHeight()).arg(dock->minimumSizeHint().height())));
        tabs->setCurrentIndex(1);
        QTest::qWait(30);
        QVERIFY2(std::abs(dock->height() - recording_height) <= 2,
                 qPrintable(QString("before=%1 after=%2 minimum=%3 hint=%4")
                     .arg(recording_height).arg(dock->height()).arg(dock->minimumHeight())
                     .arg(dock->minimumSizeHint().height())));
        tabs->setCurrentIndex(0);
        QTest::qWait(30);
        QVERIFY(std::abs(dock->height() - recording_height) <= 2);
    }
    void deep_hierarchy_breadcrumbs_remain_accessible() {
        QTemporaryDir dir;
        Project project;
        project.id = new_uuid();
        project.name = "Nested circuit with a descriptive project title";
        project.wired = true;
        std::vector<std::string> definitions(9), instances(9);
        for (auto &id : definitions) id = new_uuid();
        for (auto &id : instances) id = new_uuid();
        project.instances.push_back({instances[0], "Power stage number 1", definitions[0], 0, 0});
        for (size_t level = 0; level < definitions.size(); ++level) {
            Definition body;
            body.id = definitions[level];
            body.name = "Stage definition " + std::to_string(level + 1);
            body.wired = true;
            if (level + 1 < definitions.size())
                body.instances.push_back({instances[level + 1],
                                          "Power stage number " + std::to_string(level + 2),
                                          definitions[level + 1], 0, 0});
            project.definitions.push_back(std::move(body));
        }
        EditorWindow w("en", dir.path());
        w.set_project(project);
        ready(w);
        w.navigate_hierarchy(instances);
        QTest::qWait(30);
        auto *breadcrumb = w.findChild<QWidget *>("hierarchy_breadcrumbs");
        auto *current = w.findChild<QToolButton *>("hierarchy_level_9");
        auto *overflow = w.findChild<QToolButton *>("hierarchy_overflow");
        QVERIFY(breadcrumb && current && current->isVisible() && overflow && overflow->menu());
        QCOMPARE(overflow->menu()->actions().size(), 7);
        QVERIFY(w.width() <= 1500);
        QVERIFY(breadcrumb->rect().contains(current->geometry().center()));
        if (const auto screenshot = qEnvironmentVariable("PDS_DEEP_HIERARCHY_SCREENSHOT"); !screenshot.isEmpty())
            QVERIFY(w.grab().save(screenshot));
        overflow->menu()->actions()[3]->trigger();
        QCOMPARE(w.hierarchy_path().size(), size_t(4));
        QTest::qWait(30);
        auto *selected = w.findChild<QToolButton *>("hierarchy_level_4");
        QVERIFY(selected && selected->isVisible() && selected->isChecked());
    }
    void grid_style_settings_persist() {
        QTemporaryDir dir;
        {
            EditorWindow w("ru", dir.path());
            ready(w);
            auto *action = w.findChild<QAction *>("action_grid_settings");
            QVERIFY(action);
            QCOMPARE(action->shortcut(), QKeySequence("G"));
            bool handled = false;
            QTimer::singleShot(20, &w, [&] {
                auto *dialog = w.findChild<QDialog *>("grid_settings_dialog");
                QVERIFY(dialog);
                dialog->findChild<QComboBox *>("grid_style")->setCurrentIndex(int(Canvas::GridStyle::lines));
                dialog->findChild<QSpinBox *>("grid_size")->setValue(25);
                dialog->findChild<QDoubleSpinBox *>("grid_line_width")->setValue(2.5);
                dialog->findChild<QDoubleSpinBox *>("grid_dot_size")->setValue(3.5);
                handled = true;
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            });
            action->trigger();
            QVERIFY(handled);
            QCOMPARE(w.canvas()->grid_style(), Canvas::GridStyle::lines);
            QCOMPARE(w.canvas()->grid_size(), 25.0);
            QCOMPARE(w.canvas()->grid_line_width(), 2.5);
            QCOMPARE(w.canvas()->grid_dot_size(), 3.5);
        }
        EditorWindow restored("ru", dir.path());
        QCOMPARE(restored.canvas()->grid_style(), Canvas::GridStyle::lines);
        QCOMPARE(restored.canvas()->grid_size(), 25.0);
        QCOMPARE(restored.canvas()->grid_line_width(), 2.5);
        QCOMPARE(restored.canvas()->grid_dot_size(), 3.5);
    }

    void live_history_responsiveness_benchmark() {
        if (qEnvironmentVariableIsEmpty("PDS_LIVE_HISTORY_BENCHMARK")) QSKIP("Opt-in live GUI performance measurement");
        QTemporaryDir dir;
        EditorWindow w("ru", dir.path()); ready(w);
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/diode-freewheel.pds"));
        w.observe_object(w.project().components.front().id);
        for (const auto &wire : w.project().wires) {
            w.observe_object(wire.id);
            if (w.project().scope_points.size() >= 2)
                break;
        }
        auto *channels = w.findChild<QListWidget *>("channels");
        QVERIFY(channels && channels->count() >= 2);
        w.findChild<QTabWidget *>("results_tabs")->setCurrentIndex(1);
        w.findChild<QLineEdit *>("sim_stop")->setText("10");
        w.findChild<QLineEdit *>("sim_step")->setText("1e-6");
        for (double span : {0., .1}) {
            w.scope()->set_time_span(span);
            QElapsedTimer elapsed; elapsed.start();
            qint64 previous = 0, longest = 0;
            QTimer heartbeat; heartbeat.setInterval(10);
            connect(&heartbeat, &QTimer::timeout, &w, [&] {
                const auto now = elapsed.elapsed(); longest = std::max(longest, now - previous); previous = now;
            });
            heartbeat.start(); w.start_simulation();
            QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 45000);
            heartbeat.stop(); QVERIFY(w.has_result()); QVERIFY(!w.result().cancelled);
            QCOMPARE(w.result().last_time, 10.);
            qInfo() << "Live 10 s / 1e-6, span" << span << "wall ms" << elapsed.elapsed()
                    << "maximum GUI heartbeat gap ms" << longest << "samples" << w.result().samples.size();
        }
    }
    void example_templates_commands_and_hierarchy_buttons() {
        QTemporaryDir dir;
        EditorWindow w("ru", dir.path()); ready(w);
        const auto original = QString(PDS_SOURCE_DIR) + "/examples/diode-freewheel.pds";
        QFile source(original); QVERIFY(source.open(QIODevice::ReadOnly)); const auto bytes = source.readAll();
        QVERIFY(w.open_project(original));
        QVERIFY(w.root_project().name.find("Свободный ход") != std::string::npos);
        QVERIFY(w.suggested_save_path().endsWith("diode-freewheel-copy.pds"));
        QVERIFY(!w.save_project(original));
        source.seek(0); QCOMPARE(source.readAll(), bytes);
        const auto copy = dir.filePath("my-circuit.pds");
        QVERIFY(w.save_project(copy)); QCOMPARE(w.suggested_save_path(), copy);
        auto *example = w.findChild<QAction *>("example_diode-freewheel");
        QVERIFY(example); QVERIFY(!example->text().contains(".pds"));
        QVERIFY(qobject_cast<QMenu *>(example->parent()));
        auto *tree = w.findChild<QTreeWidget *>("library");
        QTreeWidgetItem *bridge = nullptr;
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->data(0, Qt::UserRole).isValid() && (*it)->data(0, Qt::UserRole).toInt() == 231) bridge = *it;
        QVERIFY(bridge && bridge->parent() && bridge->parent()->parent());
        QVERIFY(bridge->toolTip(0).contains("мост",Qt::CaseInsensitive));
        auto *search = w.findChild<QLineEdit *>("library_search"); search->setText(bridge->text(0));
        QVERIFY(!bridge->isHidden()); QVERIFY(bridge->parent()->isExpanded()); search->clear();
        QVERIFY(!bridge->parent()->isExpanded());
        search->setText("dead time");QVERIFY(!bridge->isHidden());search->clear();
        Project empty; empty.id = new_uuid(); empty.wired = true; w.set_project(empty);
        bool selected = false;
        QTimer::singleShot(0, &w, [&] {
            auto *dialog = w.findChild<QDialog *>("command_search_dialog");
            if (!dialog) return;
            auto *input = dialog->findChild<QLineEdit *>("command_search_input");
            input->setText(text("library") + " " + text("full_bridge_module"));
            auto *list = dialog->findChild<QListWidget *>("command_search_results");
            selected = list->currentItem() && !list->currentItem()->isHidden();
            QTest::keyClick(input, Qt::Key_Return);
            if (dialog->isVisible()) dialog->reject();
        });
        w.show_command_search(); QVERIFY(selected);
        QTest::mouseClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier, w.canvas()->mapFromScene(QPointF(0, 0)));
        QCOMPARE(w.project().instances.size(), size_t(1));
        const auto id = w.project().instances.front().id;
        QVERIFY(w.project().instances.front().locked);
        QVERIFY(definition_icon_id(w.project().instances.front().definition) == 231);
        w.select_object(id);
        auto *description=w.findChild<QLabel *>("property_description");
        QVERIFY(description&&description->isVisible()&&description->text().contains("H-мост"));
        if (!qEnvironmentVariableIsEmpty("PDS_CATALOG_SCREENSHOT")) QVERIFY(w.grab().save(qEnvironmentVariable("PDS_CATALOG_SCREENSHOT")));
        w.raise(); w.activateWindow(); QTest::qWait(50);
        auto *viewport = w.canvas()->viewport();
        for (const auto &scenePoint : {QPointF(150, 150), item(w, id)->sceneBoundingRect().center()}) {
            const auto point = w.canvas()->mapFromScene(scenePoint);
            QMouseEvent move(QEvent::MouseMove, QPointF(point), QPointF(viewport->mapToGlobal(point)),
                             Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(viewport, &move);
        }
        if (QGuiApplication::platformName() != "offscreen")
            QTRY_VERIFY(item(w, id)->toolTip().contains("data:image/png;base64"));
        if (!qEnvironmentVariableIsEmpty("PDS_CATALOG_SCREENSHOT")) QVERIFY(w.grab().save(qEnvironmentVariable("PDS_CATALOG_SCREENSHOT")));
        QTest::mouseDClick(viewport, Qt::LeftButton, Qt::NoModifier,
                          w.canvas()->mapFromScene(item(w, id)->sceneBoundingRect().center()));
        QVERIFY(w.hierarchy_path().empty());
        QTRY_COMPARE(w.hierarchy_path().size(), size_t(1));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTest::qWait(30);
        if (!qEnvironmentVariableIsEmpty("PDS_CATALOG_SCREENSHOT")) QVERIFY(w.grab().save(qEnvironmentVariable("PDS_CATALOG_SCREENSHOT") + ".hierarchy.png"));
        auto *back = w.findChild<QToolButton *>("hierarchy_level_0");
        auto *edit = w.findChild<QToolButton *>("edit_definition_button");
        QVERIFY(back && back->isVisible() && edit && edit->isVisible());
        QTest::mouseClick(back, Qt::LeftButton, Qt::NoModifier, QPoint(3, back->height()/2));
        QVERIFY(w.hierarchy_path().empty());
    }
    void code_block_example_menu_and_roundtrip() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        ready(w);
        auto *example = w.findChild<QAction *>("example_code-block-hysteresis");
        QVERIFY(example);
        QVERIFY(example->text().contains("hysteretic", Qt::CaseInsensitive));
        example->trigger();
        QCOMPARE(w.project().code_blocks.size(), size_t(1));
        QCOMPARE(w.project().code_blocks.front().inputs.size(), size_t(1));
        QCOMPARE(w.project().code_blocks.front().outputs.size(), size_t(1));
        QCOMPARE(w.project().plots.size(), size_t(1));
        QCOMPARE(w.project().scope_channels.size(), size_t(2));
        const auto before = encoded(w.project());
        const auto copy = dir.filePath("code-block-hysteresis-copy.pds");
        QVERIFY(w.save_project(copy));
        QVERIFY(w.open_project(copy));
        QCOMPARE(encoded(w.project()), before);
        w.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 10000);
        QVERIFY(w.has_result());
        QCOMPARE(w.result().last_time, .03);
        const auto gate_key = w.project().code_blocks.front().id + "/" +
                              w.project().code_blocks.front().outputs.front().id;
        const auto gate = std::find(w.result().gate_objects.begin(),
                                    w.result().gate_objects.end(), gate_key);
        QVERIFY(gate != w.result().gate_objects.end());
        const auto gate_index = size_t(std::distance(w.result().gate_objects.begin(), gate));
        size_t edges = 0;
        for (size_t i = 1; i < w.result().samples.size(); ++i)
            edges += w.result().samples[i].gates[gate_index] !=
                     w.result().samples[i - 1].gates[gate_index];
        QCOMPARE(edges, size_t(6));
    }
    void incremental_extrema_and_scope_history() {
        std::vector<double> values;
        ExtremaIndex index;
        size_t reads = 0;
        auto value = [&](size_t i) { ++reads; return values[i]; };
        for (size_t chunk = 0; chunk < 8; ++chunk) {
            for (size_t i = 0; i < 137; ++i) values.push_back(std::sin(double(values.size()) * .17));
            const auto before = reads;
            index.append(values.size(), value);
            QCOMPARE(reads - before, size_t(137));
            for (size_t a = 0; a < values.size(); a += 17)
                for (size_t b = a + 1; b <= values.size(); b += 71) {
                    auto expected = std::minmax_element(values.begin() + a, values.begin() + b);
                    auto actual = index.range(a, b, value);
                    QCOMPARE(value(actual.first), *expected.first); QCOMPARE(value(actual.second), *expected.second);
                }
        }
        Result result; result.channels = {{"a", "Trace", "V"}, {"b", "Second", "A"}};
        for (size_t i = 0; i < 1000000; ++i)
            result.samples.push_back({double(i)*1e-6,
                                      {std::sin(double(i)*.01), std::cos(double(i)*.013)}, {}});
        Scope scope; Project project; scope.resize(1200, 500); scope.set_live(true);
        scope.set_result(&result, {0}, project); scope.fit();
        QElapsedTimer timer; timer.start();
        for (int i = 0; i < 5; ++i) { scope.fit(Scope::Axes::y); auto image = scope.grab(); QVERIFY(!image.isNull()); }
        qInfo() << "Indexed million-sample fit/render, five frames (ms):" << timer.elapsed();
        result.samples.push_back({1.1, {100, 0}, {}}); scope.set_result(&result, {0}, project); scope.fit();
        QVERIFY(scope.y_high > 100);
        scope.begin = .1; scope.end = .2; scope.fit(Scope::Axes::y); QVERIFY(scope.y_high < 2);
        scope.set_live(false); result.samples.back().values[0] = 200;
        timer.restart();
        scope.set_result(&result, {0}, project); scope.fit(); QVERIFY(scope.y_high > 200);
        qInfo() << "Exact million-sample extrema build (ms):" << timer.elapsed();
        QVERIFY2(timer.elapsed() < 750, "Exact extrema construction blocked the UI excessively");
        timer.restart();
        scope.set_result(&result, {0, 1}, project);
        QVERIFY(!scope.grab().isNull());
        QVERIFY2(timer.elapsed() < 750, "Toggling a channel rebuilt the million-sample history synchronously");
        scope.set_result(&result, {0}, project);
        scope.set_curve_style({"a", CurveLine::none, 2, CurveMarker::circle, 6});
        timer.restart();
        QVERIFY(!scope.grab().isNull());
        qInfo() << "Million-sample marker-only frame (ms):" << timer.elapsed();
        QVERIFY2(timer.elapsed() < 750, "Dense point markers traversed the complete recorded history");
    }
    void open_end_library_and_run() {
        QTemporaryDir dir; EditorWindow w("ru", dir.path());
        Project empty; empty.id = new_uuid(); empty.wired = true;
        w.set_project(empty); ready(w);
        auto *insert = w.findChild<QAction *>("insert_component_270");
        QVERIFY(insert && !insert->icon().isNull()); insert->trigger();
        QTest::mouseClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                          w.canvas()->mapFromScene(QPointF(0, 0)));
        QCOMPARE(w.project().instances.size(), size_t(1));
        w.open_subcircuit(w.project().instances.front().id);
        QCOMPARE(w.project().instances.size(), size_t(2));
        w.open_subcircuit(w.project().instances.front().id);
        QCOMPARE(w.project().instances.size(), size_t(3));
        w.open_subcircuit(w.project().instances.front().id);
        QCOMPARE(w.project().components.size(), size_t(4));
        w.navigate_hierarchy({}); w.undo(); QVERIFY(w.root_project().instances.empty());
        QVERIFY(w.open_project(PDS_SOURCE_DIR "/examples/open-end-winding.pds"));
        w.canvas()->fitInView(w.canvas()->scene()->itemsBoundingRect().adjusted(-60, -60, 60, 60), Qt::KeepAspectRatio);
        w.start_simulation(); QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 3000);
        QVERIFY(w.has_result() && !w.result().samples.empty());
        const auto path = dir.filePath("dual.pds"); QVERIFY(w.save_project(path));
        const auto expected = encoded(w.root_project());
        if (auto screenshot = qEnvironmentVariable("PDS_OPEN_END_SCREENSHOT"); !screenshot.isEmpty()) {
            QTest::qWait(30); QVERIFY(w.grab().save(screenshot + ".png"));
            w.open_plot(w.project().plots.front().id);
            auto *graph = w.findChild<QDialog *>("plot_" + QString::fromStdString(w.project().plots.front().id));
            QVERIFY(graph); QTest::qWait(30); QVERIFY(graph->grab().save(screenshot + "-plot.png")); graph->close();
            w.open_subcircuit(w.project().instances.front().id);
            QTest::qWait(30); QVERIFY(w.grab().save(screenshot + "-inside.png"));
        }
        QVERIFY(w.open_project(path)); QCOMPARE(encoded(w.root_project()), expected);
    }
    void three_phase_library_parameters() {
        for (const int type : {260, 261}) {
            QTemporaryDir dir; EditorWindow w("ru", dir.path());
            Project empty; empty.id = new_uuid(); empty.wired = true;
            w.set_project(empty); ready(w);
            auto *insert = w.findChild<QAction *>("insert_component_" + QString::number(type));
            QVERIFY(insert && !insert->icon().isNull()); insert->trigger();
            QTest::mouseClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                              w.canvas()->mapFromScene(QPointF(0, 0)));
            QCOMPARE(w.project().instances.size(), size_t(1));
            const auto module = w.project().instances.front();
            const auto body = definition(w.project(), module.definition);
            QCOMPARE(body.parameters.size(), size_t(6));
            auto *value = w.findChild<QLineEdit *>("property_parameter/" + QString::fromStdString(body.parameters.front().id));
            QVERIFY(value && value->isVisible());
            value->setText("2 mF"); QTest::keyClick(value, Qt::Key_Return);
            QCOMPARE(w.project().instances.front().parameters.front().second, .002);
            w.open_subcircuit(module.id);
            QCOMPARE(w.project().instances.size(), size_t(3));
            w.open_subcircuit(w.project().instances.front().id);
            QCOMPARE(w.project().components.size(), size_t(type == 260 ? 8 : 18));
            w.navigate_hierarchy({}); w.undo();
            QVERIFY(w.project().instances.front().parameters.empty());
            if (auto screenshot = qEnvironmentVariable("PDS_THREE_PHASE_LIBRARY_SCREENSHOT"); !screenshot.isEmpty()) {
                w.select_object(module.id); QTest::qWait(30);
                QVERIFY(w.grab().save(screenshot + QString::number(type) + ".png"));
            }
        }
    }
    void three_phase_source_connection_switches_definition() {
        QTemporaryDir dir;
        EditorWindow w("ru", dir.path());
        Project empty;
        empty.id = new_uuid();
        empty.wired = true;
        w.set_project(empty);
        ready(w);
        auto *insert = w.findChild<QAction *>("insert_component_280");
        QVERIFY(insert);
        insert->trigger();
        QTest::mouseClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                          w.canvas()->mapFromScene(QPointF(0, 0)));
        QCOMPARE(w.project().instances.size(), size_t(1));
        auto stale = w.root_project();
        Definition stale_delta = stale.definitions.front();
        stale_delta.id = "eb613164-faf4-5b03-9014-806885fef344";
        stale_delta.name = "Three-phase voltage source Delta";
        stale_delta.parameters.clear();
        stale.definitions.push_back(std::move(stale_delta));
        const auto source_id = stale.instances.front().id;
        w.set_project(stale);
        ready(w);
        w.select_object(source_id);
        auto *connection = w.findChild<QComboBox *>("property_three_phase_connection");
        QVERIFY(connection && connection->isVisible());
        connection->setCurrentIndex(connection->findData(1u));
        QTest::mouseClick(w.findChild<QPushButton *>("apply_properties"), Qt::LeftButton);
        constexpr const char *star = "1a963f2c-ceb8-5cce-b927-44d735ec9e80";
        constexpr const char *delta = "eb613164-faf4-5b03-9014-806885fef344";
        QCOMPARE(w.project().instances.front().definition, std::string(delta));
        QVERIFY(std::any_of(w.project().definitions.begin(), w.project().definitions.end(),
                            [](const Definition &definition) { return definition.id == delta; }));
        const auto &updated_delta = definition(w.project(), delta);
        QCOMPARE(updated_delta.parameters.size(), size_t(4));
        QCOMPARE(updated_delta.components.size(), size_t(2));
        QCOMPARE(w.findChild<QLabel *>("property_error")->text(), QString());
        w.undo();
        QCOMPARE(w.project().instances.front().definition, std::string(star));
        w.redo();
        QCOMPARE(w.project().instances.front().definition, std::string(delta));
    }
    void ac_controller_library_and_run() {
        QTemporaryDir dir; EditorWindow w("ru", dir.path());
        Project empty; empty.id = new_uuid(); empty.wired = true;
        w.set_project(empty); ready(w);
        auto *insert = w.findChild<QAction *>("insert_component_250");
        QVERIFY(insert && !insert->icon().isNull()); insert->trigger();
        QTest::mouseClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                          w.canvas()->mapFromScene(QPointF(0, 0)));
        QCOMPARE(w.root_project().instances.size(), size_t(1));
        const auto module = w.project().instances.front();
        const auto parameter = definition(w.project(), module.definition).parameters.front().id;
        auto *value = w.findChild<QLineEdit *>("property_parameter/" + QString::fromStdString(parameter));
        QVERIFY(value && value->isVisible());
        value->setText("20 mOhm"); QTest::keyClick(value, Qt::Key_Return);
        QCOMPARE(w.project().instances.front().parameters.front().second, .02);
        w.open_subcircuit(module.id);
        QCOMPARE(w.project().components.size(), size_t(2));
        w.navigate_hierarchy({}); w.undo();
        QVERIFY(w.project().instances.front().parameters.empty());
        QVERIFY(w.open_project(PDS_SOURCE_DIR "/examples/ac-voltage-controller.pds"));
        w.canvas()->fitInView(w.canvas()->scene()->itemsBoundingRect().adjusted(-60, -60, 60, 60), Qt::KeepAspectRatio);
        w.start_simulation(); QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 3000);
        QVERIFY(w.has_result() && !w.result().samples.empty());
        const auto path = dir.filePath("ac.pds"); QVERIFY(w.save_project(path));
        const auto expected = encoded(w.root_project());
        if (auto screenshot = qEnvironmentVariable("PDS_AC_CONTROLLER_SCREENSHOT"); !screenshot.isEmpty()) {
            QTest::qWait(30); QVERIFY(w.grab().save(screenshot + ".png"));
            w.open_plot(w.project().plots.front().id);
            auto *graph = w.findChild<QDialog *>("plot_" + QString::fromStdString(w.project().plots.front().id));
            QVERIFY(graph); QTest::qWait(30); QVERIFY(graph->grab().save(screenshot + "-plot.png")); graph->close();
            w.open_subcircuit(w.project().instances.front().id);
            QTest::qWait(30); QVERIFY(w.grab().save(screenshot + "-inside.png"));
        }
        QVERIFY(w.open_project(path)); QCOMPARE(encoded(w.root_project()), expected);
    }
    void dc_link_library_and_run() {
        QTemporaryDir dir; EditorWindow w("ru", dir.path());
        Project empty; empty.id = new_uuid(); empty.wired = true; w.set_project(empty); ready(w);
        for (const int type : {240, 241, 242}) {
            ready(w);
            auto *insert = w.findChild<QAction *>("insert_component_" + QString::number(type));
            QVERIFY(insert && !insert->icon().isNull()); insert->trigger();
            QTest::mouseClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                              w.canvas()->mapFromScene(QPointF(0, 0)));
            QCOMPARE(w.project().instances.size(), size_t(1));
            const auto module = w.project().instances.front();
            const auto parameter = definition(w.project(), module.definition).parameters.front().id;
            auto *value = w.findChild<QLineEdit *>("property_parameter/" + QString::fromStdString(parameter));
            QVERIFY(value && value->isVisible());
            w.open_subcircuit(module.id); QCOMPARE(w.project().components.size(), size_t(type == 240 ? 3 : 2));
            w.navigate_hierarchy({}); w.undo(); QVERIFY(w.project().instances.empty());
            QVERIFY(w.hierarchy_path().empty());
            QVERIFY(w.root_project().instances.empty());
        }
        for (const QString kind : {QString("precharge-discharge"), QString("braking-chopper")}) {
            QVERIFY(w.open_project(QString(PDS_SOURCE_DIR "/examples/") + kind + ".pds"));
            w.canvas()->fitInView(w.canvas()->scene()->itemsBoundingRect().adjusted(-60, -60, 60, 60), Qt::KeepAspectRatio);
            w.start_simulation(); QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 3000);
            QVERIFY(w.has_result() && !w.result().samples.empty());
            const auto path = dir.filePath(kind + ".pds"); QVERIFY(w.save_project(path));
            QVERIFY(std::any_of(w.result().channels.begin(), w.result().channels.end(),
                                 [](const auto &channel) { return channel.name == "u:Udc"; }));
            const auto expected = encoded(w.root_project());
            if (auto screenshot = qEnvironmentVariable("PDS_DC_LINK_SCREENSHOT"); !screenshot.isEmpty()) {
                QTest::qWait(30); QVERIFY(w.grab().save(screenshot + kind + ".png"));
                const auto plot = w.project().plots.front().id; w.open_plot(plot);
                auto *graph = w.findChild<QDialog *>("plot_" + QString::fromStdString(plot));
                QVERIFY(graph); QTest::qWait(30); QVERIFY(graph->grab().save(screenshot + kind + "-plot.png")); graph->close();
            }
            QVERIFY(w.open_project(path)); QCOMPARE(encoded(w.root_project()), expected);
        }
    }
    void bridge_library_and_run() {
        for (const int type : {230, 231}) {
            QTemporaryDir dir; EditorWindow w("ru", dir.path());
            Project empty; empty.id = new_uuid(); empty.wired = true; w.set_project(empty); ready(w);
            auto *insert = w.findChild<QAction *>("insert_component_" + QString::number(type));
            QVERIFY(insert && !insert->icon().isNull()); insert->trigger();
            QTest::mouseClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                              w.canvas()->mapFromScene(QPointF(0, 0)));
            QCOMPARE(w.project().instances.size(), size_t(1));
            const auto module = w.project().instances.front(); w.open_subcircuit(module.id);
            if (type == 231) {
                QCOMPARE(w.project().instances.size(), size_t(2));
                w.open_subcircuit(w.project().instances.front().id);
            }
            QCOMPARE(w.project().components.size(), size_t(4));
            w.navigate_hierarchy({}); w.undo(); QVERIFY(w.project().instances.empty());
            w.redo(); QCOMPARE(w.project().instances.front().id, module.id);
            const QString kind = type == 230 ? "half-bridge" : "full-bridge";
            QVERIFY(w.open_project(QString(PDS_SOURCE_DIR "/examples/") + kind + ".pds"));
            w.canvas()->fitInView(w.canvas()->scene()->itemsBoundingRect().adjusted(-60, -60, 60, 60), Qt::KeepAspectRatio);
            w.start_simulation(); QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 2000);
            QVERIFY(w.has_result() && !w.result().samples.empty());
            const auto path = dir.filePath("bridge.pds"); QVERIFY(w.save_project(path));
            const auto expected = encoded(w.root_project());
            QVERIFY(w.open_project(path)); QCOMPARE(encoded(w.root_project()), expected);
            if (auto screenshot = qEnvironmentVariable("PDS_BRIDGE_SCREENSHOT"); !screenshot.isEmpty()) {
                QTest::qWait(30); QVERIFY(w.grab().save(screenshot + kind + ".png"));
                w.open_subcircuit(w.project().instances.front().id);
                QTest::qWait(30); QVERIFY(w.grab().save(screenshot + kind + "-inside.png"));
            }
        }
    }
    void dcdc_library_parameters_and_run() {
        for (const int type : {220, 221, 222, 223}) {
            QTemporaryDir dir;
            EditorWindow w("ru", dir.path());
            Project empty; empty.id = new_uuid(); empty.wired = true; w.set_project(empty);
            ready(w);
            auto *insert = w.findChild<QAction *>("insert_component_" + QString::number(type));
            QVERIFY(insert && !insert->icon().isNull()); insert->trigger();
            QTest::mouseClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                              w.canvas()->mapFromScene(QPointF(0, 0)));
            QCOMPARE(w.project().instances.size(), size_t(1));
            const auto module = w.project().instances.front();
            const auto parameter = definition(w.project(), module.definition).parameters.front().id;
            auto *value = w.findChild<QLineEdit *>("property_parameter/" + QString::fromStdString(parameter));
            QVERIFY(value && value->isVisible());
            value->setText("4 mH"); QTest::keyClick(value, Qt::Key_Return);
            QCOMPARE(w.project().instances[0].parameters.front().second, .004);
            w.undo(); QVERIFY(w.project().instances[0].parameters.empty());
            w.open_subcircuit(module.id); QCOMPARE(w.project().components.size(), size_t(type == 223 ? 1 : 5));
            if (type == 223) QCOMPARE(w.project().instances.size(), size_t(1));
            w.navigate_hierarchy({});
            const auto path = dir.filePath("dcdc.pds"); QVERIFY(w.save_project(path));
            const auto expected = encoded(w.root_project());
            QVERIFY(w.open_project(path)); QCOMPARE(encoded(w.root_project()), expected);
            const QString kind = type == 220 ? "buck" : type == 221 ? "boost" :
                                 type == 222 ? "buck-boost" : "bidirectional-charge";
            QVERIFY(w.open_project(QString(PDS_SOURCE_DIR "/examples/") + kind + ".pds"));
            w.canvas()->fitInView(w.canvas()->scene()->itemsBoundingRect().adjusted(-60, -60, 60, 60), Qt::KeepAspectRatio);
            w.start_simulation(); QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 3000);
            QVERIFY(w.has_result() && !w.result().samples.empty());
            if (auto screenshot = qEnvironmentVariable("PDS_DCDC_SCREENSHOT"); !screenshot.isEmpty()) {
                QTest::qWait(30); QVERIFY(w.grab().save(screenshot + kind + ".png"));
                const auto plot = w.project().plots.front().id; w.open_plot(plot);
                auto *graph = w.findChild<QDialog *>("plot_" + QString::fromStdString(plot));
                QVERIFY(graph); QTest::qWait(30); QVERIFY(graph->grab().save(screenshot + kind + "-plot.png"));
                graph->close(); w.open_subcircuit(w.project().instances.front().id);
                QTest::qWait(30); QVERIFY(w.grab().save(screenshot + kind + "-inside.png"));
            }
        }
    }
    void rectifier_library_and_run() {
        for (const int type : {210, 211, 212, 213}) {
            QTemporaryDir dir;
            EditorWindow w("ru", dir.path());
            Project empty; empty.id = new_uuid(); empty.wired = true; w.set_project(empty);
            ready(w);
            auto *insert = w.findChild<QAction *>("insert_component_" + QString::number(type));
            QVERIFY(insert && !insert->icon().isNull());
            insert->trigger();
            QTest::mouseClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                              w.canvas()->mapFromScene(QPointF(0, 0)));
            QCOMPARE(w.project().instances.size(), size_t(1));
            const auto module = w.project().instances.front();
            w.open_subcircuit(module.id);
            QCOMPARE(w.project().components.size(), size_t(type % 2 ? 6 : 4));
            w.navigate_hierarchy({});
            w.undo(); QVERIFY(w.project().instances.empty());
            w.redo(); QCOMPARE(w.project().instances.front().id, module.id);
            const auto path = dir.filePath("bridge.pds");
            QVERIFY(w.save_project(path)); const auto expected = encoded(w.root_project());
            QVERIFY(w.open_project(path)); QCOMPARE(encoded(w.root_project()), expected);
            const auto example = QString("%1-bridge-%2p").arg(type >= 212 ? "thyristor" : "diode")
                                                       .arg(type % 2 ? 3 : 1);
            QVERIFY(w.open_project(QString(PDS_SOURCE_DIR "/examples/") + example + ".pds"));
            w.canvas()->fitInView(w.canvas()->scene()->itemsBoundingRect().adjusted(-60, -60, 60, 60), Qt::KeepAspectRatio);
            w.start_simulation(); QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 2000);
            QVERIFY(w.has_result() && !w.result().samples.empty());
            if (auto screenshot = qEnvironmentVariable("PDS_RECTIFIER_SCREENSHOT"); !screenshot.isEmpty()) {
                QTest::qWait(30); QVERIFY(w.grab().save(screenshot + QString::number(type) + ".png"));
                const auto plot = w.project().plots.front().id; w.open_plot(plot);
                auto *graph = w.findChild<QDialog *>("plot_" + QString::fromStdString(plot));
                QVERIFY(graph); QTest::qWait(30);
                QVERIFY(graph->grab().save(screenshot + QString::number(type) + "-plot.png"));
                graph->close();
                w.open_subcircuit(w.project().instances.front().id);
                QTest::qWait(30); QVERIFY(w.grab().save(screenshot + QString::number(type) + "-inside.png"));
            }
        }
    }
    void transistor_library_and_run() {
        try {
            for (const int type : {200, 201}) {
                QTemporaryDir dir;
                EditorWindow w("ru", dir.path());
                Project empty; empty.id = new_uuid(); empty.wired = true; w.set_project(empty);
                ready(w);
                auto *insert = w.findChild<QAction *>("insert_component_" + QString::number(type));
                QVERIFY(insert && !insert->icon().isNull());
                insert->trigger();
                QTest::mouseClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                                  w.canvas()->mapFromScene(QPointF(0, 0)));
                QCOMPARE(w.project().instances.size(), size_t(1));
                QCOMPARE(w.project().definitions.size(), size_t(1));
                const auto module = w.project().instances.front();
                const auto ports = definition(w.project(), module.definition).ports;
                const auto parameter = definition(w.project(), module.definition).parameters.front().id;
                auto *ron = w.findChild<QLineEdit *>("property_parameter/" + QString::fromStdString(parameter));
                QVERIFY(ron && ron->isVisible());
                w.undo(); QVERIFY(w.project().instances.empty() && w.project().definitions.empty());
                w.redo(); QCOMPARE(w.project().instances.front().id, module.id);
                w.open_subcircuit(module.id);
                QCOMPARE(w.project().components.size(), size_t(2));
                w.findChild<QAction *>("edit_definition")->trigger();
                const auto diode = std::find_if(w.project().components.begin(), w.project().components.end(),
                                                [](const auto &c) { return c.kind == Kind::diode; })->id;
                w.select_object(diode);
                auto *vf = w.findChild<QLineEdit *>("property_forward_voltage");
                QVERIFY(vf);
                QVERIFY(vf->isVisible());
                // The diode voltage is a public module parameter. Its internal
                // target is intentionally read-only; edit the instance-facing
                // field so the binding remains authoritative.
                QVERIFY(vf->isReadOnly());
                w.navigate_hierarchy({});
                w.select_object(module.id);
                const auto &module_definition = definition(w.project(), module.definition);
                const auto diode_vf = std::find_if(
                    module_definition.parameters.begin(), module_definition.parameters.end(),
                    [&](const PublicParameter &candidate) {
                        return candidate.object == diode && candidate.field == "forward_voltage";
                    });
                QVERIFY(diode_vf != module_definition.parameters.end());
                auto *public_vf = w.findChild<QLineEdit *>(
                    "property_parameter/" + QString::fromStdString(diode_vf->id));
                QVERIFY(public_vf && public_vf->isVisible() && !public_vf->isReadOnly());
                const auto diode_vf_id = diode_vf->id;
                public_vf->setText("800 mV");
                QTest::keyClick(public_vf, Qt::Key_Return);
                QCOMPARE(std::get<double>(read_property(w.project(), module.id, "parameter/" + diode_vf_id)), .8);
                const auto source = w.add_component(Kind::voltage, {-300, 160});
                const auto resistor = w.add_component(Kind::resistor, {300, 160});
                const auto ground = w.add_node(true, {0, 300});
                const auto gate = w.add_pattern({-240, -160});
                w.select_object(gate);
                auto *initial = w.findChild<QCheckBox *>("property_closed");
                QVERIFY(initial && initial->isVisible());
                QTest::mouseClick(initial, Qt::LeftButton, Qt::NoModifier, QPoint(8, initial->height() / 2));
                QVERIFY(w.project().patterns.front().initial);
                QVERIFY(w.connect_ports({source, "p"}, {module.id, ports[0].id}));
                QVERIFY(w.connect_ports({module.id, ports[1].id}, {resistor, "p"}));
                QVERIFY(w.connect_ports({resistor, "n"}, {ground, "node"}));
                QVERIFY(w.connect_ports({source, "n"}, {ground, "node"}));
                QVERIFY(w.connect_ports({gate, "out"}, {module.id, ports[2].id}));
                const auto graph_id = w.add_plot({500, -160});
                QVERIFY(w.connect_ports({module.id, ports[1].id}, {graph_id, "in1"}));
                QVERIFY(w.connect_ports({gate, "out"}, {graph_id, "in2"}));
                w.start_simulation(); QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 2000);
                QVERIFY(w.has_result() && !w.result().samples.empty());
                const auto path = dir.filePath("transistor.pds");
                QVERIFY(w.save_project(path)); const auto expected = encoded(w.root_project());
                QVERIFY(w.open_project(path)); QCOMPARE(encoded(w.root_project()), expected);
                w.select_object(module.id);
                w.canvas()->fitInView(w.canvas()->scene()->itemsBoundingRect().adjusted(-60, -60, 60, 60), Qt::KeepAspectRatio);
                if (auto screenshot = qEnvironmentVariable("PDS_TRANSISTOR_SCREENSHOT"); !screenshot.isEmpty()) {
                    QTest::qWait(30); QVERIFY(w.grab().save(screenshot + QString::number(type) + ".png"));
                    w.open_subcircuit(module.id);
                    w.select_object(diode);
                    QTest::qWait(30); QVERIFY(w.grab().save(screenshot + QString::number(type) + "-inside.png"));
                }
            }
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
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
        latched = w.findChild<QCheckBox *>("property_initial_latched");
        QVERIFY(latched && latched->isVisible());
        QTest::mouseClick(latched, Qt::LeftButton, Qt::NoModifier, QPoint(8, latched->height() / 2));
        QVERIFY(device().semiconductor.initial_latched);
        w.undo(); QVERIFY(!device().semiconductor.initial_latched);
        auto *mode = w.findChild<QComboBox *>("property_semiconductor_model");
        QVERIFY(mode && mode->isVisible());
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
            auto mode = [&] { return w.findChild<QComboBox *>("property_semiconductor_model"); };
            auto line = [&](const char *name) { return w.findChild<QLineEdit *>(name); };
            auto *ron = line("property_ron");
            QVERIFY(mode() && mode()->isVisible());
            QVERIFY(!ron->isVisible());
            auto select_mode = [&](int index) {
                auto *editor = mode();
                QVERIFY(editor);
                editor->setCurrentIndex(index);
                QMetaObject::invokeMethod(editor, "activated", Q_ARG(int, index));
            };
            select_mode(1);
            ron = line("property_ron");
            auto *roff = line("property_roff");
            auto *vf = line("property_forward_voltage");
            QVERIFY(ron->isVisible() && roff->isVisible());
            QCOMPARE(vf->isVisible(), kind == Kind::diode);
            auto *charge = w.findChild<QComboBox *>("property_charge_model");
            auto *transit = line("property_transit_time");
            QCOMPARE(charge->isVisible(), kind == Kind::diode);
            QVERIFY(!transit->isVisible());
            if (kind == Kind::diode) {
                charge->setCurrentIndex(1);
                QMetaObject::invokeMethod(charge, "activated", Q_ARG(int, 1));
                transit = line("property_transit_time");
                auto *lifetime = line("property_carrier_lifetime");
                auto *initial_charge = line("property_initial_charge");
                QVERIFY(transit->isVisible() && lifetime->isVisible() && initial_charge->isVisible());
                transit->setText("100 us"); QTest::keyClick(transit, Qt::Key_Return);
                lifetime = line("property_carrier_lifetime");
                lifetime->setText("500 us"); QTest::keyClick(lifetime, Qt::Key_Return);
                initial_charge = line("property_initial_charge");
                initial_charge->setText("1 uC"); QTest::keyClick(initial_charge, Qt::Key_Return);
            }
            const auto before = encoded(w.root_project());
            ron = line("property_ron");
            ron->setText("0"); QTest::keyClick(ron, Qt::Key_Return);
            QCOMPARE(encoded(w.root_project()), before);
            QVERIFY(!w.findChild<QLabel *>("property_error")->text().isEmpty());
            ron = line("property_ron");
            ron->setText("200 mOhm"); QTest::keyClick(ron, Qt::Key_Return);
            roff = line("property_roff");
            roff->setText("100 kOhm"); QTest::keyClick(roff, Qt::Key_Return);
            if (kind == Kind::diode) {
                vf = line("property_forward_voltage");
                vf->setText("800 mV"); QTest::keyClick(vf, Qt::Key_Return);
            }
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
            select_mode(0);
            ron = line("property_ron");
            QVERIFY(!ron->isVisible());
            w.undo(); QCOMPARE(device().semiconductor.model, SemiconductorModel::piecewise_linear);
            ron = line("property_ron");
            QVERIFY(ron->isVisible());
            if (kind == Kind::diode) {
                transit = line("property_transit_time");
                QVERIFY(transit->isVisible() && device().semiconductor.charge_dynamics);
            }
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
        QVERIFY(button);
        QVERIFY(button->isVisible());
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
        button = w.findChild<QPushButton *>("import_source_points");
        table = w.findChild<QTableWidget *>("property_source_points");
        QVERIFY(button && table);
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
        QVERIFY(w.findChild<QComboBox*>("property_source_mode"));
        auto select_mode=[&](int index){
            auto *mode=w.findChild<QComboBox*>("property_source_mode");
            QVERIFY(mode&&mode->isVisible());
            mode->setCurrentIndex(index);
            QMetaObject::invokeMethod(mode,"activated",Q_ARG(int,index));
        };
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
        QVERIFY(points->isVisible());
        auto *current_frequency=w.findChild<QLineEdit*>("property_source_frequency");
        QVERIFY(!current_frequency || !current_frequency->isVisible());
        points->setItem(0,0,new QTableWidgetItem("0s"));points->setItem(0,1,new QTableWidgetItem("0V"));
        points->setItem(1,0,new QTableWidgetItem("2 ms"));points->setItem(1,1,new QTableWidgetItem("3 V"));
        QTest::mouseClick(w.findChild<QPushButton*>("apply_properties"),Qt::LeftButton);
        QCOMPARE(selected_source().source.points,(std::vector<Point>{{0,0},{.002,3}}));
        w.start_simulation();QTRY_VERIFY_WITH_TIMEOUT(!w.running(),2000);QVERIFY(w.has_result());
        if(auto path=qEnvironmentVariable("PDS_SOURCE_SCREENSHOT");!path.isEmpty())QVERIFY(w.grab().save(path));
        const auto saved=dir.filePath("source.pds");QVERIFY(w.save_project(saved));
        const auto expected=encoded(w.root_project());QVERIFY(w.open_project(saved));
        QCOMPARE(encoded(w.root_project()),expected);
        w.select_object(source);select_mode(0);
        auto *current_points=w.findChild<QTableWidget*>("property_source_points");
        QVERIFY(!current_points || !current_points->isVisible());
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
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
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
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            w.start_simulation();
            QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 10000);
            QVERIFY(w.has_result() && !w.result().samples.empty());
            if (auto folder = qEnvironmentVariable("PDS_CONVERTER_SCREENSHOT_DIR"); !folder.isEmpty()) {
                w.canvas()->fitInView(w.canvas()->scene()->itemsBoundingRect().adjusted(-50, -50, 50, 50),
                                      Qt::KeepAspectRatio);
                QVERIFY(w.grab().save(folder + "/" + example + "-leg.png"));
            }
            w.navigate_hierarchy({});
            for (auto *old_level : w.findChildren<QToolButton *>("hierarchy_level_2"))
                QVERIFY(!old_level->isVisible());
            QCoreApplication::processEvents();
            const auto root_levels = w.findChildren<QToolButton *>("hierarchy_level_0");
            QVERIFY(std::any_of(root_levels.begin(), root_levels.end(),
                                [](auto *level) { return level->isVisible(); }));
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
        QVERIFY(diagnostics->hasFocus());
        QApplication::clipboard()->clear();
        QTest::keyClick(diagnostics, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), diagnostics->item(0)->text());
        new QListWidgetItem("Second diagnostic", diagnostics);
        diagnostics->item(1)->setSelected(true);
        QApplication::clipboard()->clear();
        QTest::keyClick(diagnostics, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(),
                 diagnostics->item(0)->text() + "\nSecond diagnostic");
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
        QTRY_COMPARE(w.hierarchy_path(), std::vector<std::string>{grouped});
        QCOMPARE(w.project().components.size(), size_t(2));
        w.select_object(resistor);
        auto *value = w.findChild<QLineEdit *>("property_value");
        if (value->isReadOnly()) {
            w.findChild<QAction *>("edit_definition")->trigger();
            value = w.findChild<QLineEdit *>("property_value");
            QVERIFY(value);
        }
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
        w.canvas()->setFocus();
        w.findChild<QAction *>("hierarchy_up")->trigger();
        QVERIFY(w.hierarchy_path().empty());
        w.undo();
        QCOMPARE(w.hierarchy_path(), std::vector<std::string>{grouped});
        QCOMPARE(definition(w.root_project(), shared).components.front().value, 1000.);
        w.navigate_hierarchy({});
        w.redo();
        QCOMPARE(w.hierarchy_path(), std::vector<std::string>{grouped});
        QCOMPARE(definition(w.root_project(), shared).components.front().value, 2000.);
        w.navigate_hierarchy({});
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
    void public_interface_exposes_signal_and_tag_ports() {
        QTemporaryDir dir;
        Project project;
        project.id = new_uuid();
        project.wired = true;
        Definition body;
        body.id = new_uuid();
        body.name = "Signal subsystem";
        body.wired = true;
        CodeBlock block;
        block.id = new_uuid();
        block.name = "Logic";
        block.code = "gate = sense > 0;";
        block.inputs.push_back({new_uuid(), "sense", "V", SignalScalarType::real, 0});
        block.outputs.push_back({new_uuid(), "gate", "", SignalScalarType::boolean, 0});
        body.code_blocks.push_back(block);
        GatePattern pattern;
        pattern.id = new_uuid();
        pattern.name = "Gate C";
        pattern.script = true;
        pattern.outputs = 3;
        pattern.code = "IN[0] = 0; IN[1] = 0; IN[2] = 0;";
        body.patterns.push_back(pattern);
        ConnectionTag tag;
        tag.id = new_uuid();
        tag.name = "Signal tag";
        tag.domain = Domain::signal;
        body.tags.push_back(tag);
        project.definitions.push_back(body);
        const auto instance = new_uuid();
        project.instances.push_back({instance, "Signals", body.id, 0, 0});
        EditorWindow w("en", dir.path());
        w.set_project(project);
        ready(w);
        w.select_object(instance);
        bool visited = false;
        QTimer::singleShot(20, &w, [&] {
            auto *dialog = w.findChild<QDialog *>("public_interface_dialog");
            QVERIFY(dialog);
            auto *ports = dialog->findChild<QTableWidget *>("public_ports");
            auto *add = dialog->findChild<QPushButton *>("public_ports_add");
            QVERIFY(ports && add);
            const auto expose = [&](const QString &name, const QString &label) {
                add->click();
                const int row = ports->rowCount() - 1;
                if (row == 1)
                    QCOMPARE(ports->item(row, 0)->text(), QString("port3"));
                ports->item(row, 0)->setText(name);
                auto *binding = qobject_cast<QComboBox *>(ports->cellWidget(row, 1));
                QCOMPARE(binding->count(), 6);
                const int index = binding->findText(label);
                QVERIFY(index >= 0);
                binding->setCurrentIndex(index);
            };
            expose("port2", "Logic / sense");
            expose("gate", "Logic / gate");
            expose("third", "Gate C / out2");
            expose("tag", "Signal tag / io");
            visited = true;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        QTimer::singleShot(2000, &w, [&] {
            if (auto *dialog = w.findChild<QDialog *>("public_interface_dialog"))
                dialog->reject();
        });
        w.findChild<QAction *>("public_interface")->trigger();
        QVERIFY(visited);
        const auto ports = definition(w.root_project(), body.id).ports;
        QCOMPARE(ports.size(), size_t(4));
        QCOMPARE(ports[0].terminal, (Endpoint{block.id, block.inputs[0].id}));
        QCOMPARE(ports[0].domain, Domain::signal);
        QCOMPARE(ports[0].direction, Direction::input);
        QCOMPARE(ports[1].terminal, (Endpoint{block.id, block.outputs[0].id}));
        QCOMPARE(ports[1].domain, Domain::gate);
        QCOMPARE(ports[1].direction, Direction::output);
        QCOMPARE(ports[2].terminal, (Endpoint{pattern.id, "out2"}));
        QCOMPARE(ports[2].domain, Domain::gate);
        QCOMPARE(ports[3].terminal, (Endpoint{tag.id, "io"}));
        QCOMPARE(ports[3].domain, Domain::signal);
        QCOMPARE(ports[3].direction, Direction::conserving);
        validate_hierarchy(w.root_project());
        QCOMPARE(flatten(w.root_project()).project.tags.size(), size_t(1));
        auto connected = w.root_project();
        CodeBlock signal;
        signal.id = new_uuid();
        signal.name = "External signal";
        signal.code = "out = 1;";
        signal.outputs.push_back({new_uuid(), "out", "", SignalScalarType::real, 0});
        connected.code_blocks.push_back(signal);
        connected.wires.push_back({new_uuid(), {signal.id, signal.outputs[0].id},
                                   {instance, ports[3].id}, {}});
        QCOMPARE(flatten(connected).project.wires.size(), size_t(1));
        std::istringstream saved(encoded(w.root_project()));
        QCOMPARE(definition(read_project(saved), body.id).ports, ports);
        w.undo();
        QVERIFY(definition(w.root_project(), body.id).ports.empty());
        w.redo();
        QCOMPARE(definition(w.root_project(), body.id).ports, ports);
    }
    void public_interface_preserves_shared_source_parameters() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/library/sources/three-phase-source-y.pds"));
        ready(w);
        const auto instance = w.project().instances.front();
        const auto original = definition(w.root_project(), instance.definition).parameters;
        w.select_object(instance.id);
        bool accepted = false;
        QTimer::singleShot(20, &w, [&] {
            auto *dialog = w.findChild<QDialog *>("public_interface_dialog");
            QVERIFY(dialog);
            auto *table = dialog->findChild<QTableWidget *>("public_parameters");
            QCOMPARE(table->rowCount(), int(original.size()));
            for (int row = 0; row < table->rowCount(); ++row) {
                auto *binding = qobject_cast<QComboBox *>(table->cellWidget(row, 1));
                QVERIFY(binding);
                QCOMPARE(binding->currentText(), QString::fromStdString(original[size_t(row)].name));
            }
            accepted = true;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        QTimer::singleShot(2000, &w, [&] {
            if (auto *dialog = w.findChild<QDialog *>("public_interface_dialog"))
                dialog->reject();
        });
        w.findChild<QAction *>("public_interface")->trigger();
        QVERIFY(accepted);
        QCOMPARE(definition(w.root_project(), instance.definition).parameters, original);
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
        const auto grouped_definition = w.project().instances.front().definition;
        QImage custom_image(12, 8, QImage::Format_ARGB32_Premultiplied);
        custom_image.fill(QColor("#d34f62"));
        QByteArray custom_png;
        QBuffer custom_buffer(&custom_png);
        QVERIFY(custom_buffer.open(QIODevice::WriteOnly));
        QVERIFY(custom_image.save(&custom_buffer, "PNG"));
        auto customized = w.root_project();
        auto customized_definition = std::find_if(customized.definitions.begin(), customized.definitions.end(),
                                                   [&](const Definition &candidate) {
                                                       return candidate.id == grouped_definition;
                                                   });
        QVERIFY(customized_definition != customized.definitions.end());
        customized_definition->appearance.image_png = custom_png.toBase64().toStdString();
        QVERIFY(same_simulation(w.root_project(), customized));
        customized_definition->initialization_code = "double base_resistance = 1500;";
        w.set_project(customized);
        w.select_object(group);
        const auto before_interface = w.root_project();
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
            dialog->findChild<QTabWidget *>()->setCurrentIndex(1);
            QCoreApplication::processEvents();
            QVERIFY(dialog->width() >= 1000);
            QVERIFY(parameters->columnWidth(1) >= 160);
            parameters->item(0, 0)->setText("DC-link precharge resistance");
            const auto parameter_id = parameters->item(0, 0)->data(Qt::UserRole).toString();
            auto *binding = qobject_cast<QComboBox *>(parameters->cellWidget(0, 1));
            auto *parameter_default = qobject_cast<QLineEdit *>(parameters->cellWidget(0, 2));
            auto *minimum = qobject_cast<QLineEdit *>(parameters->cellWidget(0, 5));
            auto *maximum = qobject_cast<QLineEdit *>(parameters->cellWidget(0, 6));
            QCOMPARE(binding->objectName(), "public_parameter_binding/" + parameter_id);
            QCOMPARE(parameter_default->objectName(), "public_parameter_default/" + parameter_id);
            QCOMPARE(minimum->objectName(), "public_parameter_minimum/" + parameter_id);
            QCOMPARE(maximum->objectName(), "public_parameter_maximum/" + parameter_id);
            parameter_default->setText("2 kOhm");
            auto *calculated = parameter_default->findChild<QLabel *>("calculated_value");
            QVERIFY(calculated && !calculated->isVisible());
            parameter_default->setText("missing_name * 2");
            QVERIFY(calculated->isVisible() && calculated->text() == QString::fromUtf8("—"));
            parameters->item(0, 3)->setText("Electrical parameters of power stage");
            minimum->setText("1 kOhm");
            maximum->setText("4 kOhm");
            auto *symbol = dialog->findChild<QComboBox *>("public_symbol");
            QVERIFY(symbol);
            symbol->setCurrentIndex(symbol->findData(220));
            auto *image_preview = dialog->findChild<QLabel *>("public_image_preview");
            QVERIFY(image_preview && !image_preview->pixmap().isNull());
            auto *error = dialog->findChild<QLabel *>("public_interface_error");
            QVERIFY(error);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            QVERIFY(dialog->isVisible());
            QVERIFY(!error->text().isEmpty());
            QCOMPARE(w.root_project(), before_interface);
            parameter_default->setText("base_resistance * 2");
            QVERIFY(calculated && calculated->isVisible());
            QVERIFY(calculated->text().contains("3") && calculated->text().contains("kOhm"));
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
        QCOMPARE(d.parameters[0].value, 3000.);
        QCOMPARE(d.parameters[0].default_expression, std::string("base_resistance * 2"));
        QCOMPARE(d.parameters[0].name, std::string("DC-link precharge resistance"));
        QCOMPARE(d.parameters[0].group, std::string("Electrical parameters of power stage"));
        QVERIFY(d.parameters[0].has_minimum);
        QCOMPARE(d.parameters[0].minimum, 1000.);
        QVERIFY(d.parameters[0].has_maximum);
        QCOMPARE(d.parameters[0].maximum, 4000.);
        QCOMPARE(d.appearance.symbol, 220);
        QVERIFY(!d.appearance.image_png.empty());
        const auto public_parameter_id = d.parameters[0].id;
        const auto after_interface = w.root_project();
        w.undo();
        QCOMPARE(w.root_project(), before_interface);
        w.redo();
        QCOMPARE(w.root_project(), after_interface);
        std::ostringstream appearance_stream;
        write_project(w.root_project(), appearance_stream);
        std::istringstream appearance_input(appearance_stream.str());
        const auto restored_definition = definition(read_project(appearance_input), definition_id);
        QCOMPARE(restored_definition.appearance.symbol, 220);
        QCOMPARE(restored_definition.parameters[0].default_expression,
                 std::string("base_resistance * 2"));
        QCOMPARE(restored_definition.parameters[0].group,
                 std::string("Electrical parameters of power stage"));
        w.select_object(group);
        bool numeric_default_saved = false;
        QTimer::singleShot(20, &w, [&] {
            auto *dialog = w.findChild<QDialog *>("public_interface_dialog");
            if (!dialog)
                return;
            auto *table = dialog->findChild<QTableWidget *>("public_parameters");
            auto *parameter_default = qobject_cast<QLineEdit *>(table->cellWidget(0, 2));
            QCOMPARE(parameter_default->text(), QString("base_resistance * 2"));
            parameter_default->setText("2.5 kOhm");
            auto *symbol = dialog->findChild<QComboBox *>("public_symbol");
            QVERIFY(symbol->findData(0) >= 0);
            QVERIFY(symbol->findData(108) >= 0);
            symbol->setCurrentIndex(symbol->findData(0));
            numeric_default_saved = true;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        w.findChild<QAction *>("public_interface")->trigger();
        QVERIFY(numeric_default_saved);
        QCOMPARE(definition(w.root_project(), definition_id).parameters[0].value, 2500.);
        QVERIFY(definition(w.root_project(), definition_id).parameters[0].default_expression.empty());
        QCOMPARE(definition(w.root_project(), definition_id).appearance.symbol, 0);
        std::ostringstream atom_symbol_stream;
        write_project(w.root_project(), atom_symbol_stream);
        std::istringstream atom_symbol_input(atom_symbol_stream.str());
        QCOMPARE(definition(read_project(atom_symbol_input), definition_id).appearance.symbol, 0);
        w.undo();
        QCOMPARE(w.root_project(), after_interface);
        const auto labels = w.findChildren<QLabel *>();
        QVERIFY(std::any_of(labels.begin(), labels.end(), [](QLabel *label) {
            return label->isVisible() && label->text() == "Electrical parameters of power stage";
        }));
        auto *parameter =
            w.findChild<QLineEdit *>("property_parameter/" + QString::fromStdString(public_parameter_id));
        QVERIFY(parameter && parameter->isVisible());
        parameter->setFocus();
        parameter->selectAll();
        QTest::keyClicks(parameter, "3.5 kOhm");
        QTest::keyClick(parameter, Qt::Key_Return);
        QCOMPARE(w.project().instances.front().parameters.front().second, 3500.);
        QCOMPARE(definition(w.root_project(), definition_id).parameters[0].value, 3000.);
        w.open_subcircuit(group);
        w.select_object(resistor);
        w.findChild<QAction *>("edit_definition")->trigger();
        auto *value = w.findChild<QLineEdit *>("property_value");
        QVERIFY(value->isReadOnly());
        QCOMPARE(parse_si(value->text().toStdString(), "Ohm"), 3500.);
        QCOMPARE(definition(w.root_project(), definition_id).parameters[0].value, 3000.);
        auto flat = flatten(w.root_project()).project;
        auto component = std::find_if(flat.components.begin(), flat.components.end(), [&](const auto &c) {
            return c.id == expanded_uuid({group}, resistor);
        });
        QVERIFY(component != flat.components.end());
        QCOMPARE(component->value, 3500.);

        auto deep = w.root_project();
        auto leaf_definition = std::find_if(deep.definitions.begin(), deep.definitions.end(),
                                             [&](const Definition &candidate) {
                                                 return candidate.id == definition_id;
                                             });
        QVERIFY(leaf_definition != deep.definitions.end());
        const auto leaf_parameter = leaf_definition->parameters.front().id;
        leaf_definition->parameters.front().value = 1100.;
        Definition middle;
        middle.id = new_uuid();
        middle.name = "Middle mask";
        middle.wired = true;
        const auto leaf_instance = new_uuid();
        const auto middle_parameter = new_uuid();
        middle.instances = {{leaf_instance, "Leaf", definition_id, 0, 0}};
        middle.parameters = {{middle_parameter, "Middle resistance", "Ohm", leaf_instance,
                              leaf_parameter, 2000., "Deep mask", true, 1500., false, 0.}};
        Definition top;
        top.id = new_uuid();
        top.name = "Top mask";
        top.wired = true;
        const auto middle_instance = new_uuid();
        const auto top_parameter = new_uuid();
        top.instances = {{middle_instance, "Middle", middle.id, 0, 0}};
        top.parameters = {{top_parameter, "Top resistance", "Ohm", middle_instance,
                           middle_parameter, 2000., "Deep mask"}};
        deep.definitions.push_back(middle);
        deep.definitions.push_back(top);
        const auto middle_preview_instance = new_uuid();
        deep.instances.push_back({middle_preview_instance, "Middle preview", middle.id, 200, 200});
        const auto top_instance = new_uuid();
        deep.instances.push_back({top_instance, "Deep instance", top.id, 300, 200});
        w.set_project(std::move(deep));
        ready(w);
        w.select_object(middle_preview_instance);
        bool nested_default_checked = false;
        QTimer::singleShot(20, &w, [&] {
            auto *dialog = w.findChild<QDialog *>("public_interface_dialog");
            if (!dialog)
                return;
            auto *parameters = dialog->findChild<QTableWidget *>("public_parameters");
            dialog->findChild<QPushButton *>("public_parameters_add")->click();
            QCOMPARE(parameters->rowCount(), 1);
            parameters->selectRow(0);
            dialog->findChild<QPushButton *>("public_parameters_delete")->click();
            dialog->findChild<QPushButton *>("public_parameters_add")->click();
            auto *nested_default = qobject_cast<QLineEdit *>(parameters->cellWidget(0, 2));
            QVERIFY(nested_default);
            QCOMPARE(parse_si(nested_default->text().toStdString(), "Ohm"), 3000.);
            nested_default_checked = true;
            dialog->reject();
        });
        w.findChild<QAction *>("public_interface")->trigger();
        QVERIFY(nested_default_checked);
        w.select_object(top_instance);
        auto *deep_parameter =
            w.findChild<QLineEdit *>("property_parameter/" + QString::fromStdString(top_parameter));
        QVERIFY(deep_parameter && deep_parameter->isVisible());
        const auto before_invalid = encoded(w.root_project());
        deep_parameter->setText("4.5 kOhm");
        QTest::keyClick(deep_parameter, Qt::Key_Return);
        QCOMPARE(encoded(w.root_project()), before_invalid);
        QVERIFY(!w.findChild<QLabel *>("property_error")->text().isEmpty());
        deep_parameter->setText("1.2 kOhm");
        QTest::keyClick(deep_parameter, Qt::Key_Return);
        QCOMPARE(encoded(w.root_project()), before_invalid);
        deep_parameter->setText("2.5 kOhm");
        QTest::keyClick(deep_parameter, Qt::Key_Return);
        const auto applied = std::find_if(w.project().instances.begin(), w.project().instances.end(),
                                          [&](const Instance &instance) { return instance.id == top_instance; });
        QVERIFY(applied != w.project().instances.end());
        const std::vector<std::pair<std::string, double>> expected_parameters{{top_parameter, 2500.}};
        QCOMPARE(applied->parameters, expected_parameters);
        QVERIFY(w.findChild<QLabel *>("property_error")->text().isEmpty());
    }
    void public_parameter_binding_after_row_removal() {
        QTemporaryDir dir;
        Project project;
        project.id = new_uuid();
        project.wired = true;
        Definition body;
        body.id = new_uuid();
        body.name = "Editable mask";
        body.wired = true;
        Component resistor, capacitor;
        resistor.id = new_uuid(); resistor.name = "R"; resistor.kind = Kind::resistor; resistor.value = 1000;
        capacitor.id = new_uuid(); capacitor.name = "C"; capacitor.kind = Kind::capacitor; capacitor.value = 1e-6;
        body.components = {resistor, capacitor};
        body.parameters.push_back({new_uuid(), "Resistance", "Ohm", resistor.id, "value", 1000});
        body.parameters.push_back({new_uuid(), "Capacitance", "F", capacitor.id, "value", 1e-6});
        project.definitions.push_back(body);
        const auto instance_id = new_uuid();
        project.instances.push_back({instance_id, "Mask", body.id, 0, 0});
        EditorWindow w("en", dir.path());
        w.set_project(project);
        ready(w);
        w.select_object(instance_id);
        bool checked = false;
        QTimer::singleShot(20, &w, [&] {
            auto *dialog = w.findChild<QDialog *>("public_interface_dialog");
            if (!dialog)
                return;
            auto *parameters = dialog->findChild<QTableWidget *>("public_parameters");
            QVERIFY(parameters && parameters->rowCount() == 2);
            parameters->selectRow(0);
            dialog->findChild<QPushButton *>("public_parameters_delete")->click();
            QCOMPARE(parameters->rowCount(), 1);
            auto *binding = qobject_cast<QComboBox *>(parameters->cellWidget(0, 1));
            QVERIFY(binding);
            binding->setCurrentIndex(0);
            QVERIFY(QMetaObject::invokeMethod(binding, "activated", Qt::DirectConnection, Q_ARG(int, 0)));
            QCOMPARE(parameters->item(0, 4)->text(), QString("Ohm"));
            QCOMPARE(qobject_cast<QLineEdit *>(parameters->cellWidget(0, 2))->text(), QString("1000"));
            checked = true;
            dialog->reject();
        });
        QTimer::singleShot(2000, &w, [&] {
            if (auto *dialog = w.findChild<QDialog *>("public_interface_dialog"))
                dialog->reject();
        });
        w.findChild<QAction *>("public_interface")->trigger();
        QVERIFY(checked);
        QCOMPARE(w.root_project(), project);
    }
    void public_mask_preserves_nested_instance_override() {
        QTemporaryDir dir;
        Project project;
        project.id = new_uuid();
        project.wired = true;
        Definition leaf;
        leaf.id = new_uuid();
        leaf.name = "Leaf";
        leaf.wired = true;
        Component resistor;
        resistor.id = new_uuid();
        resistor.name = "R";
        resistor.kind = Kind::resistor;
        resistor.value = 1000;
        leaf.components.push_back(resistor);
        const auto leaf_parameter = new_uuid();
        leaf.parameters.push_back({leaf_parameter, "Resistance", "Ohm", resistor.id, "value", 1000});
        Definition parent;
        parent.id = new_uuid();
        parent.name = "Parent";
        parent.wired = true;
        const auto child = new_uuid();
        parent.instances.push_back({child, "Configured leaf", leaf.id, 0, 0});
        parent.instances.back().parameters.push_back({leaf_parameter, 2500});
        project.definitions = {leaf, parent};
        const auto parent_instance = new_uuid();
        project.instances.push_back({parent_instance, "Parent instance", parent.id, 0, 0});
        EditorWindow w("en", dir.path());
        w.set_project(project);
        ready(w);
        w.select_object(parent_instance);
        bool saved = false;
        QTimer::singleShot(20, &w, [&] {
            auto *dialog = w.findChild<QDialog *>("public_interface_dialog");
            QVERIFY(dialog);
            auto *parameters = dialog->findChild<QTableWidget *>("public_parameters");
            dialog->findChild<QPushButton *>("public_parameters_add")->click();
            QCOMPARE(parameters->rowCount(), 1);
            QCOMPARE(qobject_cast<QLineEdit *>(parameters->cellWidget(0, 2))->text(),
                     QString("2500"));
            saved = true;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        QTimer::singleShot(2000, &w, [&] {
            if (auto *dialog = w.findChild<QDialog *>("public_interface_dialog"))
                dialog->reject();
        });
        w.findChild<QAction *>("public_interface")->trigger();
        QVERIFY(saved);
        const auto &edited = definition(w.root_project(), parent.id);
        QCOMPARE(edited.parameters.size(), size_t(1));
        QCOMPARE(edited.parameters.front().value, 2500.);
        QCOMPARE(flatten(w.root_project()).project.components.front().value, 2500.);
        w.undo();
        QVERIFY(definition(w.root_project(), parent.id).parameters.empty());
        w.redo();
        QCOMPARE(flatten(w.root_project()).project.components.front().value, 2500.);

        auto expressed = project;
        expressed.definitions[1].instances.front().parameters.clear();
        expressed.definitions[1].initialization_code = "double setting = 3200;";
        expressed.definitions[1].parameter_expressions.push_back(
            {child, "parameter/" + leaf_parameter, "setting"});
        QTemporaryDir expression_dir;
        EditorWindow expression_window("en", expression_dir.path());
        expression_window.set_project(expressed);
        ready(expression_window);
        expression_window.select_object(parent_instance);
        bool expression_checked = false;
        QTimer::singleShot(20, &expression_window, [&] {
            auto *dialog = expression_window.findChild<QDialog *>("public_interface_dialog");
            QVERIFY(dialog);
            dialog->findChild<QPushButton *>("public_parameters_add")->click();
            auto *parameters = dialog->findChild<QTableWidget *>("public_parameters");
            QCOMPARE(qobject_cast<QLineEdit *>(parameters->cellWidget(0, 2))->text(), QString("3200"));
            expression_checked = true;
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        QTimer::singleShot(2000, &expression_window, [&] {
            if (auto *dialog = expression_window.findChild<QDialog *>("public_interface_dialog"))
                dialog->reject();
        });
        expression_window.findChild<QAction *>("public_interface")->trigger();
        QVERIFY(expression_checked);
        QCOMPARE(definition(expression_window.root_project(), parent.id).parameters.front().value, 3200.);
        QCOMPARE(definition(expression_window.root_project(), parent.id).parameter_expressions.size(), size_t(1));
        QCOMPARE(flatten(expression_window.root_project()).project.components.front().value, 3200.);
    }
    void public_mask_separates_ungrouped_fields() {
        QTemporaryDir dir;
        Project project;
        project.id = new_uuid();
        project.wired = true;
        Definition body;
        body.id = new_uuid();
        body.name = "Grouped mask";
        body.wired = true;
        for (const auto kind : {Kind::resistor, Kind::capacitor, Kind::inductor}) {
            Component component;
            component.id = new_uuid();
            component.name = kind_name(kind);
            component.kind = kind;
            component.value = kind == Kind::resistor ? 1000. : 1e-3;
            body.components.push_back(component);
            PublicParameter parameter;
            parameter.id = new_uuid();
            parameter.name = component.name;
            parameter.object = component.id;
            parameter.field = "value";
            parameter.value = component.value;
            parameter.group = kind == Kind::capacitor ? "" : "Power";
            body.parameters.push_back(parameter);
        }
        project.definitions.push_back(body);
        const auto instance_id = new_uuid();
        project.instances.push_back({instance_id, "Mask", body.id, 0, 0});
        EditorWindow w("en", dir.path());
        w.set_project(project);
        ready(w);
        w.select_object(instance_id);
        std::vector<QLabel *> headings;
        for (auto *label : w.findChildren<QLabel *>())
            if (label->isVisible() && (label->text() == "Power" || label->text() == "Ungrouped"))
                headings.push_back(label);
        std::sort(headings.begin(), headings.end(), [](QLabel *a, QLabel *b) {
            return a->mapToGlobal(QPoint(0, 0)).y() < b->mapToGlobal(QPoint(0, 0)).y();
        });
        QCOMPARE(headings.size(), size_t(3));
        QCOMPARE(headings[0]->text(), QString("Power"));
        QCOMPARE(headings[1]->text(), QString("Ungrouped"));
        QCOMPARE(headings[2]->text(), QString("Power"));
    }
    void public_mask_lists_active_fields_and_keeps_existing_bindings() {
        QTemporaryDir dir;
        Project project;
        project.id = new_uuid();
        project.wired = true;
        Definition body;
        body.id = new_uuid();
        body.name = "Source mask";
        body.wired = true;
        Component source;
        source.id = new_uuid();
        source.name = "V";
        source.kind = Kind::voltage;
        source.value = 10;
        body.components.push_back(source);
        project.definitions.push_back(body);
        const auto instance = new_uuid();
        project.instances.push_back({instance, "Source", body.id, 0, 0});
        EditorWindow w("en", dir.path());
        auto inspect = [&](const auto &check_dialog) {
            w.set_project(project);
            ready(w);
            w.select_object(instance);
            bool visited = false;
            QTimer::singleShot(20, &w, [&] {
                auto *dialog = w.findChild<QDialog *>("public_interface_dialog");
                if (!dialog)
                    return;
                check_dialog(dialog);
                visited = true;
                dialog->reject();
            });
            QTimer watchdog;
            watchdog.setSingleShot(true);
            QObject::connect(&watchdog, &QTimer::timeout, &w, [&] {
                if (auto *dialog = w.findChild<QDialog *>("public_interface_dialog"))
                    dialog->reject();
            });
            watchdog.start(2000);
            w.findChild<QAction *>("public_interface")->trigger();
            watchdog.stop();
            QVERIFY(visited);
        };
        inspect([&](QDialog *dialog) {
            dialog->findChild<QPushButton *>("public_parameters_add")->click();
            auto *table = dialog->findChild<QTableWidget *>("public_parameters");
            QCOMPARE(qobject_cast<QComboBox *>(table->cellWidget(0, 1))->count(), 1);
        });
        const auto frequency_parameter = new_uuid();
        project.definitions.front().parameters.push_back(
            {frequency_parameter, "Frequency", "Hz", source.id, "source_frequency", 50});
        inspect([&](QDialog *dialog) {
            auto *table = dialog->findChild<QTableWidget *>("public_parameters");
            QCOMPARE(table->rowCount(), 1);
            auto *binding = qobject_cast<QComboBox *>(table->cellWidget(0, 1));
            QCOMPARE(binding->count(), 2);
            QCOMPARE(binding->currentText(), QString("V / Frequency, Hz"));
        });
        project.definitions.front().parameters.clear();
        project.definitions.front().components.front().source.kind = Waveform::sine;
        inspect([&](QDialog *dialog) {
            dialog->findChild<QPushButton *>("public_parameters_add")->click();
            auto *table = dialog->findChild<QTableWidget *>("public_parameters");
            auto *binding = qobject_cast<QComboBox *>(table->cellWidget(0, 1));
            QVERIFY(binding->count() > 1);
            QVERIFY(binding->findText("V / Frequency, Hz") >= 0);
        });
        project.definitions.front().parameters.push_back(
            {new_uuid(), "parameter2", "V", source.id, "value", 10});
        inspect([&](QDialog *dialog) {
            auto *table = dialog->findChild<QTableWidget *>("public_parameters");
            QCOMPARE(table->rowCount(), 1);
            const auto first = qobject_cast<QComboBox *>(table->cellWidget(0, 1))->currentIndex();
            dialog->findChild<QPushButton *>("public_parameters_add")->click();
            QCOMPARE(table->rowCount(), 2);
            const auto second = qobject_cast<QComboBox *>(table->cellWidget(1, 1))->currentIndex();
            QCOMPARE(second, first + 1);
            QCOMPARE(table->item(1, 0)->text(), QString("parameter3"));
        });
        project.definitions.front().parameters = {
            {new_uuid(), "Phase", "", source.id, "source_phase_deg", 90}};
        inspect([&](QDialog *dialog) {
            auto *table = dialog->findChild<QTableWidget *>("public_parameters");
            QCOMPARE(table->rowCount(), 1);
            QCOMPARE(table->item(0, 4)->text(), QString::fromUtf8("°"));
        });
    }
    void connection_tags_distinguish_domains_on_canvas() {
        QTemporaryDir dir;
        Project project;
        project.id = new_uuid();
        project.wired = true;
        project.tags.push_back({new_uuid(), "POWER", 0, 0, Domain::electrical});
        project.tags.push_back({new_uuid(), "FIRE", 0, 100, Domain::gate});
        project.tags.push_back({new_uuid(), "MEASURE", 0, 200, Domain::signal});
        EditorWindow w("en", dir.path());
        w.set_project(project);
        ready(w);
        const auto root_levels = w.findChildren<QToolButton *>("hierarchy_level_0");
        auto root_level = std::find_if(root_levels.begin(), root_levels.end(),
                                       [](auto *level) { return level->isVisible(); });
        QVERIFY(root_level != root_levels.end());
        QVERIFY2((*root_level)->geometry().left() <= 25,
                 qPrintable(QString("root x=%1 parent width=%2")
                     .arg((*root_level)->geometry().left()).arg((*root_level)->parentWidget()->width())));
        const QString expected[] = {"#146cca", "#17866d", "#8c67c8"};
        for (size_t index = 0; index < project.tags.size(); ++index) {
            auto *tag = item(w, project.tags[index].id);
            QVERIFY(tag && tag->childItems().size() == 1);
            QCOMPARE(tag->childItems().front()->data(11).toString(), expected[index]);
        }
        if (const auto screenshot = qEnvironmentVariable("PDS_TAG_DOMAINS_SCREENSHOT"); !screenshot.isEmpty())
            QVERIFY(w.grab().save(screenshot));
        w.select_object(project.tags.back().id);
        QTest::keyClick(w.canvas(), Qt::Key_C, Qt::ControlModifier);
        QTest::keyClick(w.canvas(), Qt::Key_V, Qt::ControlModifier);
        bool preview_port = false;
        for (auto *part : w.canvas()->scene()->items())
            if (part->data(1).toString() == "preview" && part->data(2).toString() == "io") {
                QCOMPARE(part->data(11).toString(), QString("#8c67c8"));
                preview_port = true;
            }
        QVERIFY(preview_port);
        QTest::keyClick(w.canvas(), Qt::Key_Escape);
        QCOMPARE(w.project().tags.size(), size_t(3));
    }
    void connection_tag_scope_and_suggestion_visibility() {
        try {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        Project project;
        project.id = new_uuid();
        project.wired = true;
        const auto configurable = derived_uuid("configurable-tag");
        const auto visible = derived_uuid("visible-tag");
        project.tags.push_back({configurable, "CONFIGURABLE", 0, 0, Domain::signal});
        project.tags.push_back({visible, "VISIBLE", 180, 0, Domain::signal});
        w.set_project(project);
        ready(w);

        w.select_object(configurable);
        auto *scope = w.findChild<QComboBox *>("property_tag_scope");
        auto *listed = w.findChild<QCheckBox *>("property_tag_listed");
        QVERIFY(scope && listed);
        scope->setCurrentIndex(scope->findData(unsigned(TagScope::global)));
        listed->setChecked(false);
        QTest::mouseClick(w.findChild<QPushButton *>("apply_properties"), Qt::LeftButton);
        QCOMPARE(w.project().tags.front().scope, TagScope::global);
        QVERIFY(!w.project().tags.front().listed);

        w.select_object(visible);
        QCOMPARE(w.project().tags.size(), size_t(2));
        QVERIFY(w.project().tags[1].listed);
        const auto flattened = flatten(w.root_project()).project;
        QCOMPARE(flattened.tags.size(), size_t(2));
        QVERIFY(flattened.tags[1].listed);
        auto *name = w.findChild<QLineEdit *>("property_name");
        QVERIFY(name && name->completer());
        QStringList suggestions;
        const auto *model = name->completer()->model();
        for (int row = 0; row < model->rowCount(); ++row)
            suggestions.push_back(model->index(row, 0).data().toString());
        QVERIFY2(suggestions.contains("VISIBLE"), qPrintable(suggestions.join("|")));
        QVERIFY(!suggestions.contains("CONFIGURABLE"));

        std::ostringstream output;
        write_project(w.root_project(), output);
        std::istringstream input(output.str());
        const auto restored = read_project(input);
        QCOMPARE(restored.tags.front().scope, TagScope::global);
        QVERIFY(!restored.tags.front().listed);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void public_ports_move_to_all_four_edges() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        ready(w);
        const auto resistor = w.project().components[1].id;
        w.select_object(resistor);
        const auto instance_id = w.group_selection("Movable ports");
        QVERIFY(!instance_id.empty());
        const auto definition_id = w.project().instances.front().definition;
        const auto port_id = definition(w.root_project(), definition_id).ports.front().id;
        auto frame_size = [&] {
            auto *instance = item(w, instance_id);
            return instance ? instance->mapRectToScene(instance->shape().boundingRect()).size() : QSizeF();
        };
        const auto original_frame = frame_size();
        auto verify_stable_frame = [&] {
            const auto current = frame_size();
            QVERIFY(std::abs(current.width() - original_frame.width()) < 1e-6);
            QVERIFY(std::abs(current.height() - original_frame.height()) < 1e-6);
        };
        auto move_port = [&](QPointF target) -> std::optional<PublicPort> {
            w.select_object(instance_id);
            auto *instance = item(w, instance_id);
            if (!instance)
                return std::nullopt;
            QGraphicsItem *port = nullptr;
            for (auto *child : instance->childItems())
                if (child->data(2).toString().toStdString() == port_id) {
                    port = child;
                    break;
                }
            if (!port)
                return std::nullopt;
            const QRectF body = instance->shape().boundingRect();
            const QPointF local = port->pos();
            const double dx = std::abs(std::abs(local.x()) - (body.width() / 2.0 + 10.0));
            const double dy = std::abs(std::abs(local.y()) - (body.height() / 2.0 + 10.0));
            QPointF edge;
            if (dx <= dy)
                edge = {local.x() < 0 ? body.left() : body.right(), std::clamp(local.y(), body.top(), body.bottom())};
            else
                edge = {std::clamp(local.x(), body.left(), body.right()), local.y() < 0 ? body.top() : body.bottom()};
            drag(w, instance->mapToScene(edge + (local - edge) * 0.25), instance->mapToScene(target));
            return definition(w.root_project(), definition_id).ports.front();
        };
        auto port = move_port({0, -200});
        QVERIFY(port && port->has_position && port->y < 0 && std::abs(port->x) < 90);
        verify_stable_frame();
        port = move_port({200, 35});
        QVERIFY(port && port->x > 0 && std::abs(port->y) < 100);
        verify_stable_frame();
        port = move_port({0, 200});
        QVERIFY(port && port->y > 0 && std::abs(port->x) < 90);
        verify_stable_frame();
        port = move_port({-200, 35});
        QVERIFY(port && port->x < 0 && std::abs(port->y) < 100);
        verify_stable_frame();
        for (int cycle = 0; cycle < 3; ++cycle) {
            w.undo();
            verify_stable_frame();
            w.redo();
            verify_stable_frame();
        }
    }
    void transformed_public_ports_render_on_world_grid() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        w.canvas()->set_grid_size(20);
        Project project;
        project.id = new_uuid();
        project.wired = true;
        Definition definition;
        definition.id = derived_uuid("grid-definition");
        definition.name = "Grid definition";
        definition.wired = true;
        const auto positive_node = derived_uuid("grid-positive-node");
        const auto negative_node = derived_uuid("grid-negative-node");
        definition.nodes = {{positive_node, "+", false, 0, -40},
                            {negative_node, "-", false, 0, 40}};
        definition.ports = {
            {derived_uuid("grid-positive-port"), "+", {positive_node, "node"},
             Domain::electrical, Direction::conserving, true, 0, -54},
            {derived_uuid("grid-negative-port"), "-", {negative_node, "node"},
             Domain::electrical, Direction::conserving, true, 0, 54}};
        project.definitions.push_back(definition);
        Instance instance;
        instance.id = derived_uuid("grid-instance");
        instance.name = "Scaled DC-link";
        instance.definition = definition.id;
        instance.x = 200;
        instance.y = 220;
        instance.orientation.quarter_turns = 1;
        instance.orientation.scale_x = .604938271604938;
        instance.orientation.scale_y = 1.22222222222222;
        project.instances.push_back(instance);
        w.set_project(project);
        ready(w);

        auto *block = item(w, instance.id);
        QVERIFY(block);
        int ports = 0;
        for (auto *child : block->childItems())
            if (child->data(1).toString() == "port") {
                const auto point = child->scenePos();
                QVERIFY(std::abs(point.x() / 20.0 - std::round(point.x() / 20.0)) < 1e-6);
                QVERIFY(std::abs(point.y() / 20.0 - std::round(point.y() / 20.0)) < 1e-6);
                ++ports;
            }
        QCOMPARE(ports, 2);
        QCOMPARE(pds::definition(w.root_project(), definition.id).ports[0].y, -54.0);
        QCOMPARE(pds::definition(w.root_project(), definition.id).ports[1].y, 54.0);
    }
    void framed_blocks_use_half_grid_pins_and_grid_sized_frames() {
        auto close = [](double a, double b) { return std::abs(a - b) < 1e-5; };
        for (double grid : {10.0, 20.0, 30.0}) {
            QTemporaryDir dir;
            EditorWindow w("en", dir.path());
            w.canvas()->set_grid_size(grid);
            Project project;
            project.id = new_uuid();
            project.wired = true;
            Definition definition;
            definition.id = derived_uuid("frame-grid-definition-" + std::to_string(int(grid)));
            definition.name = "Grid framed block";
            definition.wired = true;
            const auto first_node = derived_uuid(definition.id + "-first-node");
            const auto second_node = derived_uuid(definition.id + "-second-node");
            definition.nodes = {{first_node, "1", false, 0, -40},
                                {second_node, "2", false, 0, 40}};
            definition.ports = {
                {derived_uuid(definition.id + "-first-port"), "1", {first_node, "node"},
                 Domain::electrical, Direction::conserving, true, 0, -54},
                {derived_uuid(definition.id + "-second-port"), "2", {second_node, "node"},
                 Domain::electrical, Direction::conserving, true, 0, 54}};
            project.definitions.push_back(definition);
            Instance instance;
            instance.id = derived_uuid(definition.id + "-instance");
            instance.name = "Grid framed block";
            instance.definition = definition.id;
            instance.x = 200;
            instance.y = 220;
            instance.orientation.scale_x = .93;
            instance.orientation.scale_y = 1.17;
            project.instances.push_back(instance);
            PlotBlock plot;
            plot.id = derived_uuid(definition.id + "-plot");
            plot.name = "Grid plot";
            plot.x = 500;
            plot.y = 220;
            plot.orientation.scale_x = 1.13;
            plot.orientation.scale_y = .91;
            project.plots.push_back(plot);
            w.set_project(project);
            ready(w);

            auto verify = [&](const std::string &id) {
                auto *block = item(w, id);
                QVERIFY(block);
                const QRectF frame = block->shape().boundingRect();
                const QRectF scene_frame = block->mapRectToScene(frame);
                QVERIFY(close(scene_frame.width() / grid, std::round(scene_frame.width() / grid)));
                QVERIFY(close(scene_frame.height() / grid, std::round(scene_frame.height() / grid)));
                QVERIFY(close((scene_frame.left() - grid / 2.0) / grid,
                              std::round((scene_frame.left() - grid / 2.0) / grid)));
                QVERIFY(close((scene_frame.top() - grid / 2.0) / grid,
                              std::round((scene_frame.top() - grid / 2.0) / grid)));
                int ports = 0;
                for (auto *pin : block->childItems()) {
                    if (pin->data(1).toString() != "port")
                        continue;
                    const QPointF local = pin->pos();
                    const std::array<QPointF, 4> edges = {
                        QPointF(frame.left(), std::clamp(local.y(), frame.top(), frame.bottom())),
                        QPointF(frame.right(), std::clamp(local.y(), frame.top(), frame.bottom())),
                        QPointF(std::clamp(local.x(), frame.left(), frame.right()), frame.top()),
                        QPointF(std::clamp(local.x(), frame.left(), frame.right()), frame.bottom())};
                    const auto edge = *std::min_element(edges.begin(), edges.end(), [&](QPointF a, QPointF b) {
                        return QLineF(local, a).length() < QLineF(local, b).length();
                    });
                    const QPointF endpoint = pin->scenePos();
                    QVERIFY(close(endpoint.x() / grid, std::round(endpoint.x() / grid)));
                    QVERIFY(close(endpoint.y() / grid, std::round(endpoint.y() / grid)));
                    QVERIFY(close(QLineF(block->mapToScene(edge), endpoint).length(), grid / 2.0));
                    ++ports;
                }
                QVERIFY(ports > 0);
            };
            verify(instance.id);
            verify(plot.id);

            if (close(grid, 20.0)) {
                auto *block = item(w, instance.id);
                auto *untouched = item(w, plot.id);
                QVERIFY(block);
                QVERIFY(untouched);
                const double old_width = block->mapRectToScene(block->shape().boundingRect()).width();
                const double old_height = block->mapRectToScene(block->shape().boundingRect()).height();
                std::map<QString, QPointF> original_ports;
                for (auto *pin : block->childItems())
                    if (pin->data(1).toString() == "port")
                        original_ports[pin->data(2).toString()] = pin->pos();
                w.select_object(instance.id);
                untouched->setSelected(true);
                const auto untouched_position = untouched->pos();
                const auto untouched_transform = untouched->transform();
                const auto untouched_model = w.project().plots.front();
                block = item(w, instance.id);
                const QRectF visible_frame = block->data(14).toRectF();
                QVERIFY(visible_frame.isValid());
                const QPointF handle = block->mapToScene(
                    {visible_frame.right(), visible_frame.center().y()});
                QPointF direction = block->mapToScene(QPointF(1, 0)) - block->mapToScene(QPointF());
                direction /= std::hypot(direction.x(), direction.y());
                drag(w, handle, handle + direction * 37.0);
                block = item(w, instance.id);
                QVERIFY(block);
                const double new_width = block->mapRectToScene(block->shape().boundingRect()).width();
                const double new_height = block->mapRectToScene(block->shape().boundingRect()).height();
                QVERIFY(!close(new_width, old_width));
                QVERIFY(close(new_height, old_height));
                QVERIFY(close(new_width / grid, std::round(new_width / grid)));
                QVERIFY(QLineF(untouched->pos(), untouched_position).length() < 1e-9);
                QVERIFY(close(untouched->transform().m11(), untouched_transform.m11()));
                QVERIFY(close(untouched->transform().m22(), untouched_transform.m22()));
                QCOMPARE(w.project().plots.front().x, untouched_model.x);
                QCOMPARE(w.project().plots.front().y, untouched_model.y);
                QCOMPARE(w.project().plots.front().orientation, untouched_model.orientation);
                verify(instance.id);
                w.undo();
                block = item(w, instance.id);
                QVERIFY(block);
                for (auto *pin : block->childItems())
                    if (pin->data(1).toString() == "port")
                        QCOMPARE(pin->pos(), original_ports.at(pin->data(2).toString()));
            }
        }
    }
    void gate_resize_handles_follow_body_and_resize_one_axis() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        const auto gate_id = w.add_pattern({200, 200});
        ready(w);
        w.select_object(gate_id);

        auto *gate = item(w, gate_id);
        QVERIFY(gate);
        const QRectF body = gate->data(15).toRectF();
        QCOMPARE(body, QRectF(-38, -22, 76, 44));
        const auto before = w.project().patterns.front().orientation;
        const QPointF handle = gate->mapToScene({body.right(), body.center().y()});
        drag(w, handle, handle + QPointF(40, 0));

        const auto after = w.project().patterns.front().orientation;
        QVERIFY(std::abs(after.scale_x - before.scale_x) > 1e-6);
        QVERIFY(std::abs(after.scale_y - before.scale_y) < 1e-6);
    }
    void plot_pins_move_to_all_edges_without_overlap() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        Project project;
        project.id = new_uuid();
        project.wired = true;
        w.set_project(project);
        const auto plot_id = w.add_plot({0, 0});
        ready(w);
        const auto simulation_before = w.project();

        auto pin_item = [&](const std::string &port_id) -> QGraphicsItem * {
            auto *plot = item(w, plot_id);
            if (!plot)
                return nullptr;
            for (auto *child : plot->childItems())
                if (child->data(1).toString() == "port" &&
                    child->data(2).toString().toStdString() == port_id)
                    return child;
            return nullptr;
        };
        auto move_pin = [&](const std::string &port_id, QPointF target) {
            w.select_object(plot_id);
            auto *plot = item(w, plot_id);
            auto *pin = pin_item(port_id);
            QVERIFY(plot);
            QVERIFY(pin);
            const QRectF body = plot->shape().boundingRect();
            const QPointF local = pin->pos();
            const std::array<QPointF, 4> edges = {
                QPointF(body.left(), std::clamp(local.y(), body.top(), body.bottom())),
                QPointF(body.right(), std::clamp(local.y(), body.top(), body.bottom())),
                QPointF(std::clamp(local.x(), body.left(), body.right()), body.top()),
                QPointF(std::clamp(local.x(), body.left(), body.right()), body.bottom())};
            const auto edge = *std::min_element(edges.begin(), edges.end(), [&](QPointF a, QPointF b) {
                return QLineF(local, a).length() < QLineF(local, b).length();
            });
            drag(w, plot->mapToScene((local + edge) / 2.0), plot->mapToScene(target));
        };
        auto position = [&](const std::string &port_id) {
            const auto &plot = w.project().plots.front();
            auto found = std::find_if(plot.pin_positions.begin(), plot.pin_positions.end(),
                                      [&](const PinPosition &pin) { return pin.port == port_id; });
            if (found == plot.pin_positions.end())
                return QPointF(qQNaN(), qQNaN());
            return QPointF(found->x, found->y);
        };
        auto on_side = [&](const std::string &port_id, int expected_side) {
            auto *plot = item(w, plot_id);
            auto *pin = pin_item(port_id);
            if (!plot || !pin)
                return false;
            const QRectF body = plot->shape().boundingRect();
            const QPointF local = pin->pos();
            const std::array<QPointF, 4> edges = {
                QPointF(body.left(), std::clamp(local.y(), body.top(), body.bottom())),
                QPointF(body.right(), std::clamp(local.y(), body.top(), body.bottom())),
                QPointF(std::clamp(local.x(), body.left(), body.right()), body.top()),
                QPointF(std::clamp(local.x(), body.left(), body.right()), body.bottom())};
            const auto found = std::min_element(edges.begin(), edges.end(), [&](QPointF a, QPointF b) {
                return QLineF(local, a).length() < QLineF(local, b).length();
            });
            return int(std::distance(edges.begin(), found)) == expected_side &&
                   std::abs(QLineF(plot->mapToScene(*found), pin->scenePos()).length() -
                            w.canvas()->grid_size() / 2.0) < 1e-5;
        };

        const auto untouched_in2 = pin_item("in2")->pos();
        move_pin("in1", {0, -120});
        QVERIFY(on_side("in1", 2));
        QCOMPARE(pin_item("in2")->pos(), untouched_in2);
        move_pin("in1", {160, 0});
        QVERIFY(on_side("in1", 1));
        QCOMPARE(pin_item("in2")->pos(), untouched_in2);
        move_pin("in1", {0, 120});
        QVERIFY(on_side("in1", 3));
        QCOMPARE(pin_item("in2")->pos(), untouched_in2);
        move_pin("in1", {-160, 0});
        QVERIFY(on_side("in1", 0));
        QCOMPARE(pin_item("in2")->pos(), untouched_in2);
        QVERIFY(same_simulation(simulation_before, w.project()));

        move_pin("in1", {0, -120});
        move_pin("in2", {20, -120});
        QVERIFY(on_side("in1", 2));
        QVERIFY(on_side("in2", 2));
        QCOMPARE(QLineF(pin_item("in1")->scenePos(), pin_item("in2")->scenePos()).length(), 20.0);
        move_pin("in2", {-160, 20});
        const auto before_collision = encoded(w.project());
        move_pin("in2", {0, -120});
        const auto first = position("in1");
        const auto second = position("in2");
        QVERIFY(first != second);
        QVERIFY(QLineF(pin_item("in1")->scenePos(), pin_item("in2")->scenePos()).length() >= 20.0 - 1e-5);
        const auto after_collision = encoded(w.project());
        w.undo();
        QCOMPARE(encoded(w.project()), before_collision);
        w.redo();
        QCOMPARE(encoded(w.project()), after_collision);

        const auto file = dir.filePath("plot-pins.pds");
        QVERIFY(w.save_project(file));
        QVERIFY(w.open_project(file));
        QCOMPARE(encoded(w.project()), after_collision);
        QCOMPARE(pin_item("in1")->pos(), first);
        QCOMPARE(pin_item("in2")->pos(), second);

        auto imported_overlap = w.project();
        imported_overlap.plots.front().pin_positions = {{"in1", 0, -60}, {"in2", 0, -60}};
        w.set_project(imported_overlap);
        ready(w);
        QVERIFY(pin_item("in1")->pos() != pin_item("in2")->pos());
    }
    void gate_output_pins_move_and_restore() {
        QTemporaryDir dir;
        EditorWindow w("en",dir.path());
        Project project;project.id=new_uuid();project.wired=true;
        GatePattern gate;gate.id=new_uuid();gate.name="Six-channel Gate";gate.script=true;
        gate.code="for (int ind=0; ind<6; ++ind) IN[ind]=ind%2;";gate.outputs=6;
        project.patterns.push_back(gate);w.set_project(project);ready(w);w.select_object(gate.id);
        auto pin=[&]() -> QGraphicsItem* {
            auto* block=item(w,gate.id);if(!block)return nullptr;
            for(auto* child:block->childItems())
                if(child->data(2).toString()=="out5")return child;
            return nullptr;
        };
        auto* block=item(w,gate.id);QVERIFY(block&&pin());
        const auto body=block->data(15).toRectF();
        const auto start=(pin()->pos()+QPointF(body.right(),std::clamp(pin()->pos().y(),body.top(),body.bottom())))/2.0;
        drag(w,block->mapToScene(start),block->mapToScene({0,body.top()-40}));
        QCOMPARE(w.project().patterns.front().pin_positions.size(),size_t(1));
        QCOMPARE(w.project().patterns.front().pin_positions.front().port,std::string("out5"));
        const auto moved=pin()->pos();QVERIFY(moved.y()<body.top());
        w.undo();QVERIFY(w.project().patterns.front().pin_positions.empty());
        w.redo();QCOMPARE(pin()->pos(),moved);
        const auto file=dir.filePath("gate-pins.pds");QVERIFY(w.save_project(file));QVERIFY(w.open_project(file));
        QCOMPARE(pin()->pos(),moved);
    }
    void gate_wire_preview_uses_gate_color() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        const auto transistor = w.add_component(Kind::igbt, {0, 0});
        ready(w);
        auto *device = item(w, transistor);
        QVERIFY(device);
        QGraphicsItem *gate = nullptr;
        for (auto *child : device->childItems())
            if (child->data(2).toString() == "gate") {
                gate = child;
                break;
            }
        QVERIFY(gate);
        QCOMPARE(QColor(gate->data(11).toString()), QColor("#17866d"));
        auto *viewport = w.canvas()->viewport();
        const QPoint start = w.canvas()->mapFromScene(gate->scenePos());
        QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(viewport, start + QPoint(80, 40), 5);
        QGraphicsPathItem *preview = nullptr;
        for (auto *graphics : w.canvas()->scene()->items())
            if (auto *path = qgraphicsitem_cast<QGraphicsPathItem *>(graphics); path && path->zValue() == 100) {
                preview = path;
                break;
            }
        QVERIFY(preview);
        QCOMPARE(preview->pen().color(), QColor("#17866d"));
        QTest::keyClick(w.canvas(), Qt::Key_Escape);
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
        const auto resistor = std::find_if(w.project().components.begin(), w.project().components.end(),
                                           [](const Component &c) { return c.kind == Kind::resistor; })
                                  ->id;
        const auto sw = std::find_if(w.project().components.begin(), w.project().components.end(),
                                     [](const Component &c) { return c.kind == Kind::ideal_switch; })
                            ->id;
        auto gate_driven = w.root_project();
        gate_driven.events.clear();
        w.set_project(std::move(gate_driven));
        w.observe_object(resistor);
        const auto graph = resolve_connections(w.project());
        const auto net = graph.nets.at(endpoint_key({resistor, "p"}));
        const auto net_wire = std::find_if(w.project().wires.begin(), w.project().wires.end(), [&](const Wire &wire) {
            auto found = graph.nets.find(endpoint_key(wire.from));
            return found != graph.nets.end() && found->second == net;
        });
        QVERIFY(net_wire != w.project().wires.end());
        w.observe_object(net_wire->id);
        const auto pattern = w.add_pattern({160, -100});
        QVERIFY(w.connect_ports({pattern, "out"}, {sw, "gate"}));
        const auto gate_wire = std::find_if(w.project().wires.begin(), w.project().wires.end(), [&](const Wire &wire) {
            return wire.from == Endpoint{sw, "gate"} || wire.to == Endpoint{sw, "gate"};
        });
        QVERIFY(gate_wire != w.project().wires.end());
        w.observe_object(gate_wire->id);
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
        QVERIFY(click_channel(resistor));
        QVERIFY(item(w, resistor)->data(channel_highlight_role).toBool());
        QVERIFY(!item(w, resistor)->isSelected());
        QCOMPARE(encoded(w.project()), before);
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
        QTest::mouseClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
        QVERIFY(!list->currentItem());
        QVERIFY(!item(w, resistor)->data(channel_highlight_role).toBool());
        const auto wires_before_delete = w.project().wires.size();
        QVERIFY(list->count() >= 2);
        list->item(0)->setSelected(true);
        list->item(1)->setSelected(true);
        list->setCurrentItem(list->item(1), QItemSelectionModel::NoUpdate);
        list->setFocus();
        const auto channels_before_delete = w.project().scope_points.size();
        QTest::keyClick(list, Qt::Key_Delete);
        QCOMPARE(w.project().scope_points.size(), channels_before_delete - 2);
        QCOMPARE(w.project().wires.size(), wires_before_delete);
        w.set_scope_enabled(false);
        QVERIFY(!item(w, resistor)->data(channel_highlight_role).toBool());
    }
    void wire_current_observation_inserts_probe() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        ready(w);
        QVERIFY(w.project().wires.size() >= 2);
        const std::vector<std::string> wires{w.project().wires[0].id, w.project().wires[1].id};
        std::vector<QPainterPath> original_paths;
        for (const auto &wire : wires) {
            auto *graphics = qgraphicsitem_cast<QGraphicsPathItem *>(item(w, wire));
            QVERIFY(graphics);
            original_paths.push_back(graphics->path());
        }
        const auto old_wires = w.project().wires.size();
        const auto old_components = w.project().components.size();
        w.observe_wires(wires, true);
        QCOMPARE(w.project().wires.size(), old_wires + 2);
        QCOMPARE(w.project().components.size(), old_components + 2);
        std::vector<std::string> probes;
        for (const auto &component : w.project().components)
            if (component.kind == Kind::current_probe)
                probes.push_back(component.id);
        QCOMPARE(probes.size(), size_t(2));
        for (size_t index = 0; index < wires.size(); ++index) {
            auto *graphics = qgraphicsitem_cast<QGraphicsPathItem *>(item(w, wires[index]));
            QVERIFY(graphics && graphics->isVisible());
            QCOMPARE(graphics->path(), original_paths[index]);
        }
        int hidden_wire_items = 0;
        for (auto *graphics : w.canvas()->scene()->items())
            if (graphics->data(1).toString() == "wire" && !graphics->isVisible())
                ++hidden_wire_items;
        QCOMPARE(hidden_wire_items, 2);
        for (const auto &probe : probes) {
            QVERIFY(std::find(w.project().scope_points.begin(), w.project().scope_points.end(), probe) !=
                    w.project().scope_points.end());
            QVERIFY(std::find(w.project().scope_channels.begin(), w.project().scope_channels.end(), probe) !=
                    w.project().scope_channels.end());
            auto *probe_item = item(w, probe);
            QVERIFY(!probe_item->isVisible());
            QVERIFY(!(probe_item->flags() & QGraphicsItem::ItemIsSelectable));
        }
        QVERIFY(w.project().scope_enabled);
        w.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 3000);
        QVERIFY(w.has_result() && !w.result().samples.empty());
        for (const auto &probe : probes)
            QVERIFY(std::any_of(w.result().channels.begin(), w.result().channels.end(),
                                [&](const Channel &channel) { return channel.object == probe; }));
        auto *channels = w.findChild<QListWidget *>("channels");
        QVERIFY(channels);
        channels->clearSelection();
        for (int i = 0; i < channels->count(); ++i)
            if (std::find(probes.begin(), probes.end(),
                          channels->item(i)->data(Qt::UserRole).toString().toStdString()) != probes.end())
                channels->item(i)->setSelected(true);
        QCOMPARE(channels->selectedItems().size(), 2);
        channels->setCurrentItem(channels->selectedItems().front(), QItemSelectionModel::NoUpdate);
        const auto rows_before_delete = channels->count();
        QTest::keyClick(channels, Qt::Key_Delete);
        QCOMPARE(channels->count(), rows_before_delete - 2);
        QCOMPARE(w.project().components.size(), old_components);
        QCOMPARE(w.project().wires.size(), old_wires);
        QVERIFY(w.canvas()->scene()->selectedItems().empty());
        for (size_t index = 0; index < wires.size(); ++index)
            QCOMPARE(static_cast<QGraphicsPathItem *>(item(w, wires[index]))->path(), original_paths[index]);
    }
    void deleting_observed_wire_removes_hidden_current_probe() {
        QTemporaryDir dir;
        EditorWindow w("en",dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        ready(w);
        const auto wire=w.project().wires.front().id;
        const auto base_components=w.project().components.size();
        const auto base_wires=w.project().wires.size();
        w.observe_wires({wire},true);
        QCOMPARE(w.project().components.size(),base_components+1);
        const auto probe=w.project().components.back().id;
        const auto observed=encoded(w.project());
        w.select_object(wire);
        item(w,wire)->setData(wire_segment_role,1);
        QTest::keyClick(w.canvas(),Qt::Key_Delete);
        QCOMPARE(encoded(w.project()),observed);
        auto *diagnostics=w.findChild<QListWidget*>("diagnostics_list");
        QVERIFY(diagnostics->count()>0);
        QVERIFY(diagnostics->item(diagnostics->count()-1)->text().contains("observed_wire_segment"));
        item(w,wire)->setData(wire_segment_role,0);
        QTest::keyClick(w.canvas(),Qt::Key_Delete);
        QCOMPARE(w.project().components.size(),base_components);
        QCOMPARE(w.project().wires.size(),base_wires-1);
        QCOMPARE(w.project().scope_points.size(),size_t(0));
        QVERIFY(std::none_of(w.project().extensions.begin(),w.project().extensions.end(),
                             [&](const std::string& record){return record.find(probe)!=std::string::npos;}));
        const auto deleted=encoded(w.project());
        w.undo();QCOMPARE(encoded(w.project()),observed);
        w.redo();QCOMPARE(encoded(w.project()),deleted);
        w.undo();
        std::string endpoint;
        for(const auto& candidate:w.project().wires)
            if(candidate.from.object==probe||candidate.to.object==probe) {
                endpoint=candidate.from.object==probe?candidate.to.object:candidate.from.object;
                break;
            }
        QVERIFY(!endpoint.empty());
        w.select_object(endpoint);
        QTest::keyClick(w.canvas(),Qt::Key_Delete);
        QVERIFY(std::none_of(w.project().components.begin(),w.project().components.end(),
                             [&](const Component& component){return component.id==probe;}));
        QVERIFY(std::none_of(w.project().extensions.begin(),w.project().extensions.end(),
                             [&](const std::string& record){return record.find(probe)!=std::string::npos;}));
        w.undo();QCOMPARE(encoded(w.project()),observed);
    }
    void copying_observed_conductor_preserves_electrical_wire() {
        QTemporaryDir dir;
        EditorWindow w("en",dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        ready(w);
        const auto original=w.project().wires.front();
        QVERIFY(original.from.object!=original.to.object);
        w.observe_wires({original.id},true);
        const auto probe=w.project().components.back().id;
        w.select_object(original.from.object);
        item(w,original.to.object)->setSelected(true);
        QTest::keyClick(w.canvas(),Qt::Key_C,Qt::ControlModifier);
        const auto *mime=QApplication::clipboard()->mimeData();
        QVERIFY(mime&&mime->hasFormat("application/x-powerdrivesim-project"));
        std::istringstream input(mime->data("application/x-powerdrivesim-project").toStdString());
        const auto fragment=read_project(input);
        QCOMPARE(fragment.wires.size(),size_t(1));
        QVERIFY(fragment.wires.front().from==original.from);
        QVERIFY(fragment.wires.front().to==original.to);
        QVERIFY(std::none_of(fragment.components.begin(),fragment.components.end(),
                             [&](const Component& component){return component.id==probe;}));
        QTest::keyClick(w.canvas(),Qt::Key_X,Qt::ControlModifier);
        QVERIFY(std::none_of(w.project().components.begin(),w.project().components.end(),
                             [&](const Component& component){return component.id==probe;}));
        w.undo();
        QVERIFY(std::any_of(w.project().components.begin(),w.project().components.end(),
                            [&](const Component& component){return component.id==probe;}));
    }
    void selected_wire_group_is_available_from_background_menu() {
        QTemporaryDir dir;
        EditorWindow w("ru", dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        ready(w);
        QVERIFY(w.project().wires.size() >= 2);
        const auto first = w.project().wires[0].id;
        const auto second = w.project().wires[1].id;
        item(w, first)->setSelected(true);
        item(w, second)->setSelected(true);
        QCOMPARE(w.canvas()->scene()->selectedItems().size(), 2);
        bool handled = false;
        QTimer::singleShot(20, &w, [&] {
            auto *menu = w.findChild<QMenu *>("element_context");
            QVERIFY(menu);
            QMenu *observe = nullptr;
            for (auto *entry : menu->actions())
                if (entry->menu()) {
                    observe = entry->menu();
                    break;
                }
            QVERIFY(observe);
            for (auto *entry : observe->actions())
                if (entry->text() == text("observe_wire_current")) {
                    handled = true;
                    entry->trigger();
                    break;
                }
            menu->close();
        });
        const QPoint local(4, 4);
        QContextMenuEvent event(QContextMenuEvent::Mouse, local,
                                w.canvas()->viewport()->mapToGlobal(local));
        QApplication::sendEvent(w.canvas()->viewport(), &event);
        QVERIFY(handled);
        QCOMPARE(std::count_if(w.project().components.begin(), w.project().components.end(),
                               [](const Component &component) { return component.kind == Kind::current_probe; }),
                 2);
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
        w.canvas()->scene()->clearSelection();
        l->setSelected(true);
        const QPointF original = l->pos();
        auto label_hit = [&] {
            const auto bounds = l->boundingRect();
            return l->mapToScene(QPointF(bounds.right() - 2, bounds.top() + 2));
        };
        drag(w, label_hit(), label_hit() + QPointF(80, 40), Qt::AltModifier);
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
        l = label();
        QVERIFY(l);
        QCOMPARE(l->pos(), original);
        w.canvas()->scene()->clearSelection();
        l->setSelected(true);
        auto *vp = w.canvas()->viewport();
        auto hit = label_hit();
        QTest::mousePress(vp, Qt::LeftButton, Qt::AltModifier, w.canvas()->mapFromScene(hit));
        QTest::mouseMove(vp, w.canvas()->mapFromScene(hit + QPointF(60, 60)), 5);
        QTest::keyClick(w.canvas(), Qt::Key_Space);
        QTest::mouseRelease(vp, Qt::LeftButton, Qt::AltModifier,
                            w.canvas()->mapFromScene(hit + QPointF(60, 60)));
        QCOMPARE(w.project().labels.size(), size_t(1));
        QCOMPARE(w.project().labels[0].orientation.quarter_turns, 1u);
        w.undo();
        l = label();
        QVERIFY(l);
        QCOMPARE(l->pos(), original);
        QVERIFY(w.project().labels.empty());
        w.canvas()->scene()->clearSelection();
        l->setSelected(true);
        drag(w, label_hit(), label_hit() + QPointF(40, 40), Qt::AltModifier);
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
        auto *trigger_action=nav->findChild<QAction *>("trigger");
        QVERIFY(trigger_action&&!trigger_action->icon().isNull());
        trigger_action->trigger();
        auto *dialog = s.findChild<QDialog *>("scope_measurements");
        QVERIFY(dialog);
        QCOMPARE(dialog->findChild<QTabWidget *>("measurement_tabs")->currentWidget()->objectName(),QString("trigger_page"));
        QCOMPARE(dialog->findChild<QComboBox *>("trigger_channel")->count(),2);
        QVERIFY(dialog->findChild<QTableWidget *>("measurement_statistics")->rowCount() > 5);
        s.set_live(false);
        dialog->findChild<QLineEdit *>("trigger_level")->setText("5");
        auto *tabs = dialog->findChild<QTabWidget *>("measurement_tabs");
        tabs->setCurrentIndex(3);
        dialog->findChild<QPushButton *>("trigger_apply")->click();
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
    void scope_spectrum_and_harmonics() {
        init_language("en");
        Result r;
        r.channels = {{"u", "Voltage", "V"}};
        const double pi = std::acos(-1.);
        for (int k = 0; k <= 4096; ++k) {
            const double t = k / 4096.;
            r.samples.push_back({t, {2 * std::cos(2 * pi * 8 * t) + .2 * std::cos(2 * pi * 24 * t)}, {}});
        }
        Project p;
        p.scope_end = 1;
        Scope s;
        s.resize(850, 500);
        s.set_result(&r, {0}, p);
        s.show();
        auto *nav = s.navigation();
        nav->findChild<QAction *>("spectrum")->trigger();
        auto *d = s.findChild<QDialog *>("scope_spectrum");
        QVERIFY(d);
        auto *frequency = d->findChild<QLineEdit *>("spectrum_fundamental");
        auto *calculate = d->findChild<QToolButton *>("spectrum_calculate");
        frequency->setText("8 Hz");
        calculate->click();
        auto *table = d->findChild<QTableWidget *>("spectrum_harmonic_table");
        QCOMPARE(table->rowCount(), 40);
        QCOMPARE(table->item(0, 1)->text(), QString("8"));
        QCOMPARE(table->item(2, 3)->text(), QString("10"));
        QVERIFY(d->findChild<QLabel *>("spectrum_status")->text().contains("THD: 10 %"));
        auto *plot = d->findChild<Scope *>("spectrum_plot");
        QVERIFY(plot);
        QCOMPARE(plot->end, 2048.);
        // The shared navigation uses Hz, with no time-window/trigger controls.
        QVERIFY(!d->findChild<QLineEdit *>("scope_time_span"));
        QVERIFY(!d->findChild<QAction *>("cursor_mode"));
        d->findChild<QAction *>("zoom_xy")->trigger();
        QTest::qWait(30);
        QTest::mousePress(plot, Qt::LeftButton, Qt::NoModifier, {120, 50});
        QTest::mouseRelease(plot, Qt::LeftButton, Qt::NoModifier, {450, 220});
        QVERIFY(plot->end - plot->begin < 2048);
        d->findChild<QAction *>("fit_xy")->trigger();
        QCOMPARE(plot->end, 2048.);
        d->findChild<QLineEdit *>("spectrum_end")->setText("0.93");
        d->findChild<QCheckBox *>("spectrum_whole_periods")->setChecked(false);
        calculate->click();
        QCOMPARE(table->rowCount(), 0);
        QVERIFY(d->findChild<QLabel *>("spectrum_status")->text().contains("unavailable"));
        d->findChild<QLineEdit *>("spectrum_end")->setText("0");
        calculate->click();
        QVERIFY(d->findChild<QLabel *>("spectrum_status")->text().contains("Check the interval"));
        d->findChild<QLineEdit *>("spectrum_end")->setText("1");
        d->findChild<QCheckBox *>("spectrum_whole_periods")->setChecked(true);
        calculate->click();
        // Focus the useful harmonic band for the real-window visual check.
        plot->end = 80;
        plot->update();
        auto screenshot = qEnvironmentVariable("PDS_SPECTRUM_SCREENSHOT_PATH");
        if (!screenshot.isEmpty()) {
            QTest::qWait(80);
            QVERIFY(d->grab().save(screenshot));
        }
        delete nav;
    }
    void scope_time_statistics_and_energy() {
        init_language("en");
        Result r;
        r.channels = {{"u", "Voltage", "V"}, {"i", "Current", "A"}};
        for (double t : {0., .1, .9, 1.1, 2.4, 3.})
            r.samples.push_back({t, {t, 2 * t - 2}, {}});
        Project p;
        p.scope_end = 3;
        Scope s;
        s.resize(850, 500);
        s.set_result(&r, {0, 1}, p);
        s.show();
        s.show_measurements();
        auto *d = s.findChild<QDialog *>("scope_measurements");
        QVERIFY(d);
        auto *tabs = d->findChild<QTabWidget *>("measurement_tabs");
        auto *range = d->findChild<QComboBox *>("measurement_range");
        range->setCurrentIndex(2);
        auto *stats = d->findChild<QTableWidget *>("measurement_statistics");
        auto row_value = [](QTableWidget *table, const QString &label) {
            for (int row = 0; row < table->rowCount(); ++row)
                if (table->item(row, 0)->text() == label)
                    return table->item(row, 1)->text();
            return QString();
        };
        QCOMPARE(row_value(stats, "Mean"), QString("1.5 V"));
        QCOMPARE(row_value(stats, "Integral"), QString::fromUtf8("4.5 V·s"));
        d->findChild<QComboBox *>("measurement_weighting")->setCurrentIndex(1);
        QCOMPARE(row_value(stats, "Mean"), QString("1.25 V"));
        tabs->setCurrentIndex(4);
        auto *energy = d->findChild<QTableWidget *>("measurement_energy");
        QCOMPARE(row_value(energy, "Signed energy"), QString("9 J"));
        QCOMPARE(row_value(energy, "Mean power"), QString("3 W"));
        d->findChild<QCheckBox *>("measurement_reverse_current")->setChecked(true);
        QCOMPARE(row_value(energy, "Signed energy"), QString("-9 J"));
        d->findChild<QCheckBox *>("measurement_reverse_current")->setChecked(false);
        s.set_cursor(0, .2);
        s.set_cursor(1, 2.7);
        range->setCurrentIndex(1);
        // Signal cursors snap to recorded samples: 0.1 s and 3 s here.
        QCOMPARE(row_value(energy, "Signed energy"), QString("9.0093333 J"));
        d->findChild<QComboBox *>("measurement_channel")->setCurrentIndex(1);
        QCOMPARE(energy->rowCount(), 1);
        QVERIFY(energy->item(0, 1)->text().contains("Select voltage"));
        d->findChild<QComboBox *>("measurement_channel")->setCurrentIndex(0);
        range->setCurrentIndex(2);
        auto screenshot = qEnvironmentVariable("PDS_ENERGY_SCREENSHOT_PATH");
        if (!screenshot.isEmpty()) {
            QTest::qWait(100);
            QVERIFY(d->grab().save(screenshot));
        }
        s.set_result(&r, {0}, p);
        s.show_measurements();
        d = s.findChild<QDialog *>("scope_measurements");
        d->findChild<QTabWidget *>("measurement_tabs")->setCurrentIndex(4);
        QCOMPARE(d->findChild<QComboBox *>("measurement_current")->count(), 0);
        QCOMPARE(d->findChild<QTableWidget *>("measurement_energy")->rowCount(), 1);
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
        auto *navigation=s.navigation();navigation->show();
        auto *stop_toolbar=navigation->findChild<QAction *>("trigger_stop_toolbar");
        auto *level_toolbar=navigation->findChild<QLineEdit *>("trigger_level_toolbar");
        auto *position_toolbar=navigation->findChild<QLineEdit *>("trigger_position_toolbar");
        QVERIFY(stop_toolbar&&level_toolbar&&position_toolbar);
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
        QCOMPARE(d->findChild<QPushButton *>("trigger_apply")->text(),QString("Apply"));
        const auto measurement_buttons=d->findChildren<QPushButton *>();
        QVERIFY(std::none_of(measurement_buttons.begin(),measurement_buttons.end(),
                            [](QPushButton *button){return button->text()=="Refresh";}));
        d->findChild<QTabWidget *>("measurement_tabs")->setCurrentIndex(3);
        d->findChild<QLineEdit *>("trigger_level")->setText("0.5");
        d->findChild<QLineEdit *>("trigger_holdoff")->setText("3");
        auto *trigger_mode=d->findChild<QComboBox *>("trigger_mode");
        QCOMPARE(trigger_mode->itemData(0).toInt(),1);
        QCOMPARE(trigger_mode->itemData(1).toInt(),2);
        QCOMPARE(trigger_mode->itemData(2).toInt(),0);
        QCOMPARE(trigger_mode->currentIndex(),0);
        trigger_mode->setCurrentIndex(0);
        auto arm = [&] {
            d->findChild<QPushButton *>("trigger_apply")->click();
        };
        arm();
        QVERIFY(stop_toolbar->isEnabled());
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
        const QRectF trigger_area(58,12,s.width()-84,s.height()-64);
        const double level_y=std::clamp(trigger_area.bottom()-(.5-s.y_low)/(s.y_high-s.y_low)*trigger_area.height(),
                                        trigger_area.top(),trigger_area.bottom());
        QTest::mousePress(&s,Qt::LeftButton,Qt::NoModifier,{int(trigger_area.right()-2),int(level_y)});
        QTest::mouseMove(&s,{int(trigger_area.right()-2),int(trigger_area.top()+trigger_area.height()*.25)});
        QTest::mouseRelease(&s,Qt::LeftButton,Qt::NoModifier,{int(trigger_area.right()-2),int(trigger_area.top()+trigger_area.height()*.25)});
        const double dragged_level=level_toolbar->text().toDouble(),expected_level=s.y_high-(s.y_high-s.y_low)*.25;
        QVERIFY2(std::abs(dragged_level-expected_level)<.05,
                 qPrintable(QString("dragged=%1 expected=%2").arg(dragged_level).arg(expected_level)));
        const int position_start=int(trigger_area.left()+trigger_area.width()*.2);
        const int position_end=int(trigger_area.left()+trigger_area.width()*.65);
        QTest::mousePress(&s,Qt::LeftButton,Qt::NoModifier,{position_start,int(trigger_area.top()+4)});
        QTest::mouseMove(&s,{position_end,int(trigger_area.top()+4)});
        QTest::mouseRelease(&s,Qt::LeftButton,Qt::NoModifier,{position_end,int(trigger_area.top()+4)});
        QString position_text=position_toolbar->text();position_text.remove('%');
        QVERIFY(std::abs(position_text.trimmed().toDouble()-65)<1);
        stop_toolbar->trigger();QVERIFY(!stop_toolbar->isEnabled());
        trigger_mode->setCurrentIndex(1);
        arm();
        for (int i = 8; i <= 12; ++i)
            r.samples.push_back({double(i), {0}, {}});
        s.set_result(&r, {0}, p);
        QCOMPARE(s.end, 12.);
        trigger_mode->setCurrentIndex(2);
        d->findChild<QComboBox *>("trigger_edge")->setCurrentIndex(2);
        arm();
        r.samples.push_back({13, {1}, {}});
        s.set_result(&r, {0}, p);
        first = s.begin;
        r.samples.push_back({14, {0}, {}});
        r.samples.push_back({15, {1}, {}});
        s.set_result(&r, {0}, p);
        QCOMPARE(s.begin, first);
        s.fit(Scope::Axes::x);
        const double fitted=s.begin;
        r.samples.push_back({16, {0}, {}});
        r.samples.push_back({17, {1}, {}});
        s.set_result(&r, {0}, p);
        QCOMPARE(s.begin,fitted);
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
    void adaptive_step_settings_and_run() {
        QTemporaryDir dir;
        EditorWindow w("ru", dir.path());
        ready(w);
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        w.observe_object(w.project().wires.front().id);
        auto *channels = w.findChild<QListWidget *>("channels");
        QVERIFY(channels && channels->count() == 1);
        bool edited = false;
        QTimer::singleShot(0, &w, [&] {
            auto *dialog = w.findChild<QDialog *>("step_settings_dialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto *adaptive = dialog->findChild<QCheckBox *>("adaptive_enabled");
            auto *maximum = dialog->findChild<QLineEdit *>("step_maximum");
            auto *minimum = dialog->findChild<QLineEdit *>("step_minimum");
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            QVERIFY(!minimum->isEnabled());
            adaptive->setChecked(true);
            QVERIFY(minimum->isEnabled());
            maximum->setText("1 ms"); minimum->setText("1 s");
            buttons->button(QDialogButtonBox::Ok)->click();
            QVERIFY(!dialog->findChild<QLabel *>("step_error")->text().isEmpty());
            minimum->setText("1 ns");
            dialog->findChild<QLineEdit *>("step_relative")->setText("1e-5");
            if (!qEnvironmentVariableIsEmpty("PDS_ADAPTIVE_SCREENSHOT"))
                QVERIFY(dialog->grab().save(qEnvironmentVariable("PDS_ADAPTIVE_SCREENSHOT")));
            buttons->button(QDialogButtonBox::Ok)->click();
            edited = true;
        });
        w.show_step_settings();
        QVERIFY(edited);
        QVERIFY(w.project().profile.step_control.adaptive);
        QCOMPARE(w.project().profile.step, .001);
        QCOMPARE(w.findChild<QLineEdit *>("sim_step")->text(), QString("0.001"));
        w.undo(); QVERIFY(!w.project().profile.step_control.adaptive);
        w.redo(); QVERIFY(w.project().profile.step_control.adaptive);
        w.step_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 3000);
        QVERIFY(w.result().rejected_steps > 0);
        const auto rejections = w.result().rejected_steps, solves = w.result().linear_solves;
        w.step_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 3000);
        QCOMPARE(w.result().accepted_steps, size_t(2));
        QVERIFY(w.result().rejected_steps >= rejections && w.result().linear_solves > solves);
        w.continue_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 3000);
        QVERIFY(w.result().accepted_steps > 5 && w.result().max_local_error <= 1);
        QVERIFY(w.save_project(dir.filePath("adaptive.pds")));
        QVERIFY(w.open_project(dir.filePath("adaptive.pds")));
        QVERIFY(w.project().profile.step_control.adaptive);
        QCOMPARE(w.project().profile.step_control.minimum_step, 1e-9);
        QTimer::singleShot(0, &w, [&] {
            auto *dialog = w.findChild<QDialog *>("step_settings_dialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            dialog->findChild<QLineEdit *>("step_minimum")->setText("-1");
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            buttons->button(QDialogButtonBox::Ok)->click();
            QVERIFY(!dialog->findChild<QLabel *>("step_error")->text().isEmpty());
            dialog->findChild<QCheckBox *>("adaptive_enabled")->setChecked(false);
            buttons->button(QDialogButtonBox::Ok)->click();
        });
        w.show_step_settings();
        QVERIFY(!w.project().profile.step_control.adaptive);
        QCOMPARE(w.project().profile.step_control.minimum_step, 1e-9);
    }
    void initial_state_settings_and_run() {
        QTemporaryDir dir;
        EditorWindow w("ru", dir.path());
        ready(w);
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        w.observe_object(w.project().wires.front().id);
        auto *channels = w.findChild<QListWidget *>("channels");
        QVERIFY(channels && channels->count() == 1);
        bool edited = false;
        QTimer::singleShot(0, &w, [&] {
            auto *dialog = w.findChild<QDialog *>("initial_settings_dialog");
            QVERIFY(dialog);
            QTimer::singleShot(3000, dialog, &QDialog::reject);
            auto *mode = dialog->findChild<QComboBox *>("initial_state_mode");
            auto *warmup = dialog->findChild<QLineEdit *>("initial_warmup");
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            mode->setCurrentIndex(2);
            warmup->setText("1");
            buttons->button(QDialogButtonBox::Ok)->click();
            QVERIFY(dialog->isVisible());
            QVERIFY(!dialog->findChild<QLabel *>("initial_error")->text().isEmpty());
            warmup->selectAll();
            QTest::keyClicks(warmup, "0,001");
            QCOMPARE(warmup->text(), QString("0.001"));
            if (!qEnvironmentVariableIsEmpty("PDS_INITIAL_STATE_SCREENSHOT"))
                QVERIFY(dialog->grab().save(qEnvironmentVariable("PDS_INITIAL_STATE_SCREENSHOT")));
            buttons->button(QDialogButtonBox::Ok)->click();
            edited = true;
        });
        w.show_initial_settings();
        QVERIFY(edited);
        QCOMPARE(w.project().profile.initial_state, InitialState::dc_operating_point);
        QCOMPARE(w.project().profile.warmup, .001);
        w.undo();
        QCOMPARE(w.project().profile.initial_state, InitialState::specified);
        QCOMPARE(w.project().profile.warmup, 0.0);
        w.redo();
        w.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 3000);
        QVERIFY(w.has_result());
        QCOMPARE(w.result().samples.front().time, .001);
        for (const auto &sample : w.result().samples)
            for (size_t i = 0; i < sample.values.size(); ++i)
                QVERIFY(std::abs(sample.values[i] - w.result().samples.front().values[i]) < 1e-10);
        QVERIFY(w.save_project(dir.filePath("initial.pds")));
        QVERIFY(w.open_project(dir.filePath("initial.pds")));
        QCOMPARE(w.project().profile.initial_state, InitialState::dc_operating_point);
        QCOMPARE(w.project().profile.warmup, .001);
        auto p = w.root_project();
        p.profile.stop = 1; p.profile.step = 1e-7; p.profile.warmup = .9;
        w.set_project(p);
        w.start_simulation();
        QTest::qWait(80);
        QVERIFY(w.running());
        QVERIFY(!w.has_result()); // No history allocation during the warm-up interval.
        w.stop_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 2000);
        QVERIFY(!w.simulation_snapshot());
        QVERIFY(w.result().cancelled && w.result().samples.empty());
    }
    void expression_initialization_and_numeric_properties() {
        QTemporaryDir dir;
        EditorWindow w("ru",dir.path());ready(w);
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR)+"/examples/rc.pds"));
        auto *right_tabs=w.findChild<QTabWidget *>("right_workspace_tabs");
        auto *variables=w.findChild<QTableWidget *>("workspace_variables");
        QVERIFY(right_tabs&&variables);QCOMPARE(right_tabs->count(),1);
        bool edited=false;
        QTimer::singleShot(0,&w,[&]{
            auto *dialog=w.findChild<QDialog *>("expression_settings_dialog");QVERIFY(dialog);
            QTimer::singleShot(3000,dialog,&QDialog::reject);
            auto *code=dialog->findChild<QPlainTextEdit *>("expression_initialization_code");QVERIFY(code);
            auto *c_editor=dynamic_cast<CCodeEdit *>(code);QVERIFY(c_editor&&c_editor->code_completer());
            code->setPlainText("double scale(double value) {\nreturn value * 2;\n}");
            dialog->findChild<QPushButton *>("format_initialization_code")->click();
            QVERIFY(code->toPlainText().contains("\n    return value * 2;\n"));
            code->setPlainText("sq");code->moveCursor(QTextCursor::End);
            QTest::keyClick(code,Qt::Key_Space,Qt::ControlModifier);
            QVERIFY(c_editor->code_completer()->completionCount()>0);
            code->setPlainText("double ulim = 10;\n");code->moveCursor(QTextCursor::End);
            QTest::keyClicks(code,"u");
            QTRY_VERIFY(c_editor->code_completer()->completionCount()>0);
            QTest::keyClicks(code,"d");
            QTRY_COMPARE(c_editor->code_completer()->completionCount(),0);
            QVERIFY(!c_editor->code_completer()->popup()->isVisible());
            code->setPlainText("double scale(double value) { return value * 2; }\n"
                               "double base = 2e3;\ndouble factor = 0;\n"
                               "for (int i = 0; i < 2; ++i) factor += 0.5;\n"
                               "if (factor == 1) factor = scale(factor);");
            auto *compile=dialog->findChild<QPushButton *>("compile_initialization_code");QVERIFY(compile);
            compile->click();
            QCOMPARE(dialog->findChild<QLabel *>("expression_error")->text(),QString("Код успешно скомпилирован."));
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();edited=true;
        });
        w.show_expression_settings();QVERIFY(edited);
        QCOMPARE(variables->rowCount(),2);
        QMap<QString,QString> workspace;
        for(int row=0;row<variables->rowCount();++row)
            workspace[variables->item(row,0)->text()]=variables->item(row,1)->text();
        QCOMPARE(workspace.value("base"),QString("2000"));
        QCOMPARE(workspace.value("factor"),QString("2"));
        const auto resistor=std::find_if(w.project().components.begin(),w.project().components.end(),
                                         [](const Component &component){return component.kind==Kind::resistor;})->id;
        w.select_object(resistor);
        QCOMPARE(right_tabs->count(),2);
        QCOMPARE(right_tabs->tabText(0),QString("Переменные"));
        QCOMPARE(right_tabs->tabText(1),QString("Свойства"));
        auto *property_scroll=w.findChild<QScrollArea *>("property_scroll_area");
        auto *description=w.findChild<QLabel *>("property_description");
        auto *native_type=w.findChild<QLabel *>("property_native_type");
        QVERIFY(property_scroll&&property_scroll->widgetResizable());
        QVERIFY(description&&native_type&&description->geometry().top()<native_type->geometry().top());
        auto *value=w.findChild<QLineEdit *>("property_value");QVERIFY(value);
        QVERIFY(value->completer());
        value->clear();QTest::keyClicks(value,"b");
        QTRY_VERIFY(value->completer()->completionCount()>0);
        value->setText("base * factor");
        auto *calculated=value->findChild<QLabel *>("calculated_value");
        QVERIFY(calculated&&calculated->isVisible()&&!calculated->text().isEmpty());
        QCOMPARE(value->height(),34);
        QVERIFY(value->textMargins().right()>=calculated->width());
        QTest::mouseClick(w.findChild<QPushButton *>("apply_properties"),Qt::LeftButton);
        QCOMPARE(w.project().components[1].value,4000.0);
        QCOMPARE(w.project().parameter_expressions.size(),size_t(1));
        for(int row=0;row<variables->rowCount();++row)
            if(variables->item(row,0)->text()=="base")variables->item(row,1)->setText("3000");
        QVERIFY(QString::fromStdString(w.project().initialization_code).contains("base = 3000"));
        w.undo();QVERIFY(QString::fromStdString(w.project().initialization_code).contains("base = 2e3"));
        w.redo();QVERIFY(QString::fromStdString(w.project().initialization_code).contains("base = 3000"));
        QVERIFY(w.save_project(dir.filePath("expressions.pds")));
        QVERIFY(w.open_project(dir.filePath("expressions.pds")));
        w.select_object(resistor);
        QCOMPARE(w.findChild<QLineEdit *>("property_value")->text(),QString("base * factor"));
        value=w.findChild<QLineEdit *>("property_value");value->setText("3 kOhm");
        QTest::mouseClick(w.findChild<QPushButton *>("apply_properties"),Qt::LeftButton);
        QCOMPARE(w.project().components[1].value,3000.0);
        QVERIFY(w.project().parameter_expressions.empty());
        w.canvas()->scene()->clearSelection();
        QTRY_COMPARE(right_tabs->count(),1);
    }
    void gate_code_compile_button() {
        QTemporaryDir dir;
        EditorWindow w("en",dir.path());ready(w);
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR)+"/examples/gate-script-pwm.pds"));
        QVERIFY(!w.project().patterns.empty());w.select_object(w.project().patterns.front().id);
        auto *code=w.findChild<QPlainTextEdit *>("property_gate_code");
        auto *compile=w.findChild<QPushButton *>("compile_gate_code");
        auto *error=w.findChild<QLabel *>("property_error");
        auto *format=w.findChild<QPushButton *>("format_gate_code");
        auto *c_editor=dynamic_cast<CCodeEdit *>(code);
        QVERIFY(code&&compile&&compile->isVisible()&&format&&format->isVisible()&&error);
        QVERIFY(c_editor&&c_editor->code_completer());
        bool full_editor_opened=false;
        QTimer::singleShot(0,&w,[&] {
            auto* dialog=w.findChild<QDialog*>("full_gate_code_dialog");QVERIFY(dialog);
            auto* full=dialog->findChild<QPlainTextEdit*>("full_gate_code");QVERIFY(full);
            full->setPlainText("return true;");
            dialog->findChild<QPushButton*>("full_gate_code_compile")->click();
            QCOMPARE(dialog->findChild<QLabel*>("full_gate_code_status")->text(),QString("Code compiled successfully."));
            full_editor_opened=true;dialog->accept();
        });
        QTest::mouseClick(c_editor->viewport(),Qt::LeftButton,Qt::NoModifier,{c_editor->viewport()->width()-16,16});
        QVERIFY(full_editor_opened);QCOMPARE(code->toPlainText(),QString("return true;"));
        code->setPlainText("if (t > 0) {\nreturn true;\n}");
        format->click();
        QVERIFY(code->toPlainText().contains("\n    return true;\n"));
        code->setPlainText("ph");code->moveCursor(QTextCursor::End);
        QTest::keyClick(code,Qt::Key_Space,Qt::ControlModifier);
        QVERIFY(c_editor->code_completer()->completionCount()>0);
        code->setPlainText("double threshold(double x) { if (x > 0.1) return 1; return 0; } return threshold(t);");
        compile->click();QCOMPARE(error->text(),QString("Code compiled successfully."));
        code->setPlainText("double curr_ramp = ramp(0, 10, 0.008333333, 0.001111111); "
                           "return phasepwm(50, 0.02, curr_ramp) && stime == t;");
        compile->click();QCOMPARE(error->text(),QString("Code compiled successfully."));
        code->setPlainText("while (true) {} return false;");
        compile->click();QVERIFY2(error->text().contains("budget"),qPrintable(error->text()));
        auto* outputs=w.findChild<QSpinBox*>("property_gate_outputs");
        QVERIFY(outputs);outputs->setValue(6);
        code->setPlainText("for (int ind = 0; ind < 6; ++ind) IN[ind] = phasepwm(50, 0.02, ind / 300.0);");
        compile->click();QCOMPARE(error->text(),QString("Code compiled successfully."));
        QTest::mouseClick(w.findChild<QPushButton*>("apply_properties"),Qt::LeftButton);
        QCOMPARE(w.project().patterns.front().outputs,6u);
        w.select_object(w.project().patterns.front().id);
        code=w.findChild<QPlainTextEdit*>("property_gate_code");
        compile=w.findChild<QPushButton*>("compile_gate_code");
        error=w.findChild<QLabel*>("property_error");
        auto* updated_editor=dynamic_cast<CCodeEdit*>(code);QVERIFY(updated_editor);
        code->setPlainText("I");code->moveCursor(QTextCursor::End);
        QTest::keyClick(code,Qt::Key_Space,Qt::ControlModifier);
        QVERIFY(updated_editor->code_completer()->completionCount()>0);
        QVERIFY(!w.findChild<QLineEdit *>("property_script_step"));
    }
    void scope_teardown_clears_channel_list_before_events() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        ready(w);
        const auto wire = w.project().wires.front().id;
        auto check_teardown = [&](const std::function<void()> &remove_scope) {
            w.observe_object(wire);
            auto *channels = w.findChild<QListWidget *>("channels");
            QVERIFY(channels);
            bool event_during_destruction = false;
            connect(channels, &QObject::destroyed, &w, [&] {
                QEvent focus(QEvent::FocusIn);
                QCoreApplication::sendEvent(w.canvas()->viewport(), &focus);
                event_during_destruction = true;
            });
            remove_scope();
            QVERIFY(event_during_destruction);
        };
        check_teardown([&] { w.set_project(w.root_project()); });
        check_teardown([&] { w.set_scope_enabled(false); });
    }
    void simulation_snapshots_and_step() {
        QTemporaryDir dir;
        EditorWindow w("ru", dir.path());
        ready(w);
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        w.observe_object(w.project().wires.front().id);
        auto *list = w.findChild<QListWidget *>("channels");
        QVERIFY(list && list->count());
        auto *step = w.findChild<QAction *>("action_simulation_step");
        auto *resume = w.findChild<QAction *>("action_continue_state");
        auto *save = w.findChild<QAction *>("action_snapshot_save");
        QVERIFY(step && resume && save);
        QVERIFY(step->isEnabled());
        QVERIFY(!resume->isEnabled() && !save->isEnabled());
        step->trigger();
        QVERIFY(w.running());
        QVERIFY(!step->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 3000);
        QVERIFY(w.simulation_snapshot());
        QCOMPARE(w.result().accepted_steps, size_t(1));
        QCOMPARE(w.result().samples.size(), size_t(2));
        QVERIFY(resume->isEnabled() && save->isEnabled());
        step->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 3000);
        QCOMPARE(w.result().accepted_steps, size_t(2));
        QCOMPARE(w.result().samples.size(), size_t(3));
        const auto checkpoint = *w.simulation_snapshot();
        if (!qEnvironmentVariableIsEmpty("PDS_SNAPSHOT_SCREENSHOT")) {
            w.canvas()->fitInView(w.canvas()->scene()->itemsBoundingRect().adjusted(-60, -60, 60, 60), Qt::KeepAspectRatio);
            w.findChild<QTabWidget *>("results_tabs")->setCurrentIndex(1);
            QVERIFY(w.grab().save(qEnvironmentVariable("PDS_SNAPSHOT_SCREENSHOT") + ".step.png"));
        }
        const auto path = dir.filePath("state.pdss");
        QVERIFY(w.save_simulation_snapshot(path));
        Recording recording;
        recording.all = false;
        recording.channels = w.project().scope_channels;
        const auto expected = execute(compile(w.root_project()), nullptr, nullptr, &recording);
        resume->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 3000);
        QCOMPARE(w.result().accepted_steps, expected.accepted_steps);
        QCOMPARE(w.result().samples.size(), expected.samples.size());
        for (size_t i = 0; i < expected.samples.size(); ++i) {
            QCOMPARE(w.result().samples[i].time, expected.samples[i].time);
            QCOMPARE(w.result().samples[i].values, expected.samples[i].values);
        }
        QVERIFY(w.load_simulation_snapshot(path));
        QVERIFY(!w.has_result());
        QCOMPARE(*w.simulation_snapshot(), checkpoint);
        w.continue_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 3000);
        QCOMPARE(w.result().samples.size(), expected.samples.size() - 2);
        for (size_t i = 0; i < w.result().samples.size(); ++i) {
            QCOMPARE(w.result().samples[i].time, expected.samples[i + 2].time);
            QCOMPARE(w.result().samples[i].values, expected.samples[i + 2].values);
        }
        const auto completed = *w.simulation_snapshot();
        w.findChild<QLineEdit *>("sim_step")->setText("2e-6");
        QVERIFY(!w.load_simulation_snapshot(path));
        QCOMPARE(*w.simulation_snapshot(), completed);
        w.findChild<QLineEdit *>("sim_step")->setText(QString::number(w.project().profile.step, 'g', 17));
        w.start_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 3000);
        QCOMPARE(w.result().samples.front().time, 0.0);
        if (!qEnvironmentVariableIsEmpty("PDS_SNAPSHOT_SCREENSHOT"))
            QVERIFY(w.grab().save(qEnvironmentVariable("PDS_SNAPSHOT_SCREENSHOT")));
        QVERIFY(step->isEnabled());
        step->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 3000);
        QCOMPARE(w.result().accepted_steps, size_t(1));
        QCOMPARE(w.result().samples.front().time, 0.0);
        w.set_project(w.root_project());
        QVERIFY(!w.simulation_snapshot());
        QVERIFY(!save->isEnabled() && !resume->isEnabled());
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/diode-freewheel.pds"));
        const auto switch_id = std::find_if(w.project().components.begin(), w.project().components.end(),
                                            [](const Component &component) {
                                                return component.kind == Kind::ideal_switch;
                                            })
                                   ->id;
        auto gate_driven = w.root_project();
        gate_driven.events.clear();
        w.set_project(std::move(gate_driven));
        const auto gate_source = w.add_pattern({160, -100});
        QVERIFY(w.connect_ports({gate_source, "out"}, {switch_id, "gate"}));
        const auto gate_wire = std::find_if(w.project().wires.begin(), w.project().wires.end(), [&](const Wire &wire) {
            return port_type(w.project(), wire.from).domain == Domain::gate ||
                   port_type(w.project(), wire.to).domain == Domain::gate;
        });
        QVERIFY(gate_wire != w.project().wires.end());
        w.observe_object(gate_wire->id);
        list = w.findChild<QListWidget *>("channels");
        bool gate_selected = false;
        for (int i = 0; i < list->count(); ++i) {
            const bool gate = list->item(i)->data(Qt::UserRole).toString().startsWith("gate/");
            list->item(i)->setCheckState(gate ? Qt::Checked : Qt::Unchecked);
            gate_selected |= gate;
        }
        QVERIFY(gate_selected);
        for (int i = 0; i < 2; ++i) {
            w.step_simulation();
            QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 3000);
        }
        QVERIFY(!w.result().gate_objects.empty());
        QCOMPARE(w.result().samples.size(), size_t(3));
        QCOMPARE(w.result().accepted_steps, size_t(2));
    }
    void live_simulation_pause_and_stop() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        ready(w);
        QVERIFY(w.open_project(QString(PDS_SOURCE_DIR) + "/examples/rc.pds"));
        w.observe_object(w.project().wires.front().id);
        auto *list = w.findChild<QListWidget *>("channels");
        QVERIFY(list && list->count());
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
        const auto paused_time = w.result().samples.back().time;
        QTest::qWait(150);
        QCOMPARE(w.result().samples.size(), size);
        w.step_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 2000);
        QVERIFY(w.result().samples.size() > size);
        QVERIFY(w.result().samples.back().time > paused_time);
        w.continue_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(w.result().samples.size() > size, 2000);
        w.start_simulation();
        w.stop_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 2000);
        QVERIFY(w.result().cancelled);
        for (size_t i = 1; i < w.result().samples.size(); ++i)
            QVERIFY(w.result().samples[i].time > w.result().samples[i - 1].time);
        QVERIFY(!w.simulation_snapshot());
        w.step_simulation();
        QTRY_VERIFY_WITH_TIMEOUT(!w.running(), 2000);
        QCOMPARE(w.result().accepted_steps, size_t(1));
        QVERIFY(w.result().samples.size() <= 2);
        QVERIFY(!w.result().cancelled);
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
            QCOMPARE(tree->topLevelItemCount(), 5);
            const auto all_actions = w.findChildren<QAction *>();
            const auto fixed_actions = std::count_if(all_actions.begin(), all_actions.end(),
                                                     [](const QAction *action) {
                                                         return action->property("fixed").toBool();
                                                     });
            QCOMPARE(bar->actions().size(), fixed_actions + 2); // Init action and its separator.
            auto *initialization = w.findChild<QAction *>("action_expression_settings");
            QVERIFY(initialization);
            QCOMPARE(bar->actions().front(), initialization);
            bool initialization_opened = false;
            QTimer::singleShot(0, &w, [&] {
                auto *dialog = w.findChild<QDialog *>("expression_settings_dialog");
                QVERIFY(dialog);
                initialization_opened = true;
                dialog->reject();
            });
            initialization->trigger();
            QVERIFY(initialization_opened);
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
        const auto all_actions = w.findChildren<QAction *>();
        const auto fixed_actions = std::count_if(all_actions.begin(), all_actions.end(),
                                                 [](const QAction *action) {
                                                     return action->property("fixed").toBool();
                                                 });
        QCOMPARE(w.findChild<QToolBar *>("component_bar")->actions().size(), fixed_actions + 2);
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
        QTest::keyClick(c, Qt::Key_Tab);
        QCOMPARE(wire->data(wire_segment_role).toInt(), 0);
        QCOMPARE(wire->pen().color(), QColor("#e88b22"));
        QTest::mouseClick(vp, Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(wire->data(wire_segment_role).toInt(), 2);
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
    void wire_over_frame_edge_never_resizes_block() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        Project project;
        project.id = new_uuid();
        project.wired = true;
        PlotBlock plot;
        plot.id = derived_uuid("wire-over-frame-plot");
        plot.name = "Plot";
        plot.x = 0;
        plot.y = 0;
        project.plots.push_back(plot);
        const auto left = derived_uuid("wire-over-frame-left");
        const auto right = derived_uuid("wire-over-frame-right");
        project.nodes = {{left, "", false, -200, 0}, {right, "", false, 200, 0}};
        project.wires.push_back({derived_uuid("wire-over-frame-wire"),
                                 {left, "node"}, {right, "node"}, {{-60, 0}, {60, 0}}});
        w.set_project(project);
        ready(w);
        auto *block = item(w, plot.id);
        auto *wire = static_cast<QGraphicsPathItem *>(item(w, project.wires.front().id));
        QVERIFY(block && wire);
        w.select_object(plot.id);
        const auto original_transform = block->transform();
        const auto original_position = block->pos();
        const auto frame = block->boundingRect();
        const auto resize_handle = block->mapToScene(QPointF(frame.right(), frame.center().y()));
        QPainterPath crossing_path(resize_handle - QPointF(80, 0));
        crossing_path.lineTo(resize_handle + QPointF(80, 0));
        wire->setPath(crossing_path);
        const auto crossing = resize_handle;
        QTest::mouseClick(w.canvas()->viewport(), Qt::LeftButton, Qt::NoModifier,
                          w.canvas()->mapFromScene(crossing));
        QVERIFY(wire->isSelected());
        QCOMPARE(block->transform(), original_transform);
        QCOMPARE(block->pos(), original_position);
        drag(w, crossing, crossing + QPointF(0, 40));
        QCOMPARE(block->transform(), original_transform);
        QCOMPARE(block->pos(), original_position);
    }
    void deleting_complete_wire_segment_leaves_no_orphan_nodes() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        const auto left = w.add_component(Kind::resistor, {0, 0});
        const auto right = w.add_component(Kind::resistor, {240, 0});
        QVERIFY(w.connect_ports({left, "n"}, {right, "p"}));
        ready(w);
        auto *canvas = w.canvas();
        const auto middle = canvas->mapFromScene(QPointF(120, 0));
        QTest::mouseClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, middle);
        QTest::mouseClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, middle);
        QTest::keyClick(canvas, Qt::Key_Delete);
        QVERIFY(w.project().wires.empty());
        QVERIFY(w.project().nodes.empty());

        Project legacy;
        legacy.id = new_uuid();
        legacy.wired = true;
        legacy.nodes.push_back({new_uuid(), "N", false, 120, 80});
        w.set_project(legacy);
        QVERIFY(w.project().nodes.empty());
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
        // Route cleanup may remove a now-collinear original vertex, but the
        // transformed route must keep the x=40 corridor and leave the rotated
        // terminal orthogonally.
        QCOMPARE(w.project().wires[0].bends[0].y, w.port_position({a, "n"}).y());
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
    void aligned_route_is_straight() {
        const auto straight = orthogonal_route({0, 0}, {-20, 0}, {0, 100}, {0, 100}, {});
        QCOMPARE(straight.elementCount(), 2);
        QCOMPARE(QPointF(straight.elementAt(0).x, straight.elementAt(0).y), QPointF(0, 0));
        QCOMPARE(QPointF(straight.elementAt(1).x, straight.elementAt(1).y), QPointF(0, 100));
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

        // Permanent regression: dragging the straight branch segment must move
        // the actual T-junction even when the trunk is horizontal only because
        // of its own bends (the remote endpoints are on another row).
        Project branch;
        branch.id = new_uuid();
        branch.wired = true;
        EditorWindow routed("en", dir.path());
        routed.set_project(branch);
        const auto left = routed.add_node(false, {0, 80});
        const auto joint = routed.add_node(false, {200, 0});
        const auto right = routed.add_node(false, {400, 80});
        const auto graph = routed.add_plot({260, -180});
        QVERIFY(routed.connect_ports({left, "node"}, {joint, "node"}));
        QVERIFY(routed.connect_ports({joint, "node"}, {right, "node"}));
        QVERIFY(routed.connect_ports({graph, "in1"}, {joint, "node"}));
        auto routed_project = routed.project();
        routed_project.wires[0].bends = {{0, 0}};
        routed_project.wires[1].bends = {{400, 0}};
        const auto branch_wire = routed_project.wires[2].id;
        routed.set_project(routed_project);

        EditorWindow moved_block("en", dir.path());
        moved_block.set_project(routed_project);
        ready(moved_block);
        const auto before_block_drag = encoded(moved_block.project());
        drag(moved_block, {260, -180}, {200, -180});
        const auto block_joint = std::find_if(moved_block.project().nodes.begin(), moved_block.project().nodes.end(),
                                               [&](const Node &node) { return node.id == joint; });
        QVERIFY(block_joint != moved_block.project().nodes.end());
        QCOMPARE(QPointF(block_joint->x, block_joint->y), QPointF(140, 0));
        for (const auto &wire_model : moved_block.project().wires) {
            if (wire_model.from.object != joint && wire_model.to.object != joint)
                continue;
            const auto path = static_cast<QGraphicsPathItem *>(item(moved_block, wire_model.id))->path();
            const auto endpoint = wire_model.from.object == joint ? path.elementAt(0)
                                                                  : path.elementAt(path.elementCount() - 1);
            QCOMPARE(QPointF(endpoint.x, endpoint.y), QPointF(140, 0));
        }
        const auto after_block_drag = encoded(moved_block.project());
        moved_block.undo();
        QCOMPARE(encoded(moved_block.project()), before_block_drag);
        moved_block.redo();
        QCOMPARE(encoded(moved_block.project()), after_block_drag);

        ready(routed);
        const auto before_segment_drag = encoded(routed.project());
        routed.select_object(branch_wire);
        auto *branch_item = static_cast<QGraphicsPathItem *>(item(routed, branch_wire));
        const auto middle = branch_item->path().pointAtPercent(.5);
        drag(routed, middle, middle + QPointF(-60, 0));
        const auto shifted_joint = std::find_if(routed.project().nodes.begin(), routed.project().nodes.end(),
                                                 [&](const Node &node) { return node.id == joint; });
        QVERIFY(shifted_joint != routed.project().nodes.end());
        QCOMPARE(shifted_joint->x, 140.0);
        QCOMPARE(shifted_joint->y, 0.0);
        const auto optimized = branch_item->path();
        QCOMPARE(optimized.elementCount(), 3);
        const auto before_end = optimized.elementAt(optimized.elementCount() - 2);
        const auto end = optimized.elementAt(optimized.elementCount() - 1);
        QCOMPARE(before_end.x, end.x); // No hidden horizontal tail over the trunk.
        QCOMPARE(end.x, 140.0);
        QCOMPARE(end.y, 0.0);
        for (const auto &wire_model : routed.project().wires) {
            if (wire_model.from.object != joint && wire_model.to.object != joint)
                continue;
            const auto path = static_cast<QGraphicsPathItem *>(item(routed, wire_model.id))->path();
            const auto endpoint = wire_model.from.object == joint ? path.elementAt(0)
                                                                  : path.elementAt(path.elementCount() - 1);
            QCOMPARE(QPointF(endpoint.x, endpoint.y), QPointF(140, 0));
        }
        QVERIFY(std::none_of(routed.project().nodes.begin(), routed.project().nodes.end(), [&](const Node &node) {
            if (node.ground)
                return false;
            return std::none_of(routed.project().wires.begin(), routed.project().wires.end(),
                                [&](const Wire &wire_model) {
                                    return wire_model.from.object == node.id || wire_model.to.object == node.id;
                                });
        }));
        resolve_connections(routed.project());
        const auto after_segment_drag = encoded(routed.project());
        routed.undo();
        QCOMPARE(encoded(routed.project()), before_segment_drag);
        routed.redo();
        QCOMPARE(encoded(routed.project()), after_segment_drag);
        const auto routed_file = dir.filePath("moved-branch.pds");
        QVERIFY(routed.save_project(routed_file));
        QVERIFY(routed.open_project(routed_file));
        QCOMPARE(encoded(routed.project()), after_segment_drag);
        const auto reopened = static_cast<QGraphicsPathItem *>(item(routed, branch_wire))->path();
        QCOMPARE(reopened, optimized);
    }
    void moved_plot_branch_at_trunk_corner_repositions_node() {
        QTemporaryDir dir;
        EditorWindow w("en", dir.path());
        Project project;project.id=new_uuid();project.wired=true;
        w.set_project(project);
        const auto left=w.add_node(false,{0,140});
        const auto joint=w.add_node(false,{140,140});
        const auto right=w.add_node(false,{280,180});
        const auto graph=w.add_plot({320,-70});
        QVERIFY(w.connect_ports({left,"node"},{joint,"node"}));
        QVERIFY(w.connect_ports({joint,"node"},{right,"node"}));
        QVERIFY(w.connect_ports({graph,"in1"},{joint,"node"}));
        auto schematic=w.project();
        schematic.wires[1].bends={{140,180}};
        schematic.wires[2].bends={{140,-20}};
        const auto branch=schematic.wires[2].id;
        auto block_schematic=schematic;
        block_schematic.plots.front().x=200;
        EditorWindow moved_block("en",dir.path());
        moved_block.set_project(block_schematic);
        ready(moved_block);
        const auto before_block=encoded(moved_block.project());
        drag(moved_block,{200,-70},{160,-70});
        QCOMPARE(moved_block.project().plots.front().x,160.0);
        const auto block_joint=std::find_if(moved_block.project().nodes.begin(),moved_block.project().nodes.end(),
                                             [&](const Node& node){return node.id==joint;});
        QVERIFY(block_joint!=moved_block.project().nodes.end());
        QCOMPARE(QPointF(block_joint->x,block_joint->y),QPointF(100,140));
        for(const auto& wire:moved_block.project().wires) {
            if(wire.from.object!=joint&&wire.to.object!=joint)continue;
            const auto route=static_cast<QGraphicsPathItem*>(item(moved_block,wire.id))->path();
            const auto endpoint=wire.from.object==joint?route.elementAt(0):route.elementAt(route.elementCount()-1);
            QCOMPARE(QPointF(endpoint.x,endpoint.y),QPointF(100,140));
        }
        const auto after_block=encoded(moved_block.project());
        moved_block.undo();QCOMPARE(encoded(moved_block.project()),before_block);
        moved_block.redo();QCOMPARE(encoded(moved_block.project()),after_block);
        w.set_project(schematic);
        ready(w);
        w.select_object(branch);
        const auto before=encoded(w.project());
        auto *branch_item=static_cast<QGraphicsPathItem*>(item(w,branch));
        auto path=branch_item->path();
        QVERIFY(path.elementCount()>=3);
        QCOMPARE(path.elementAt(path.elementCount()-2).x,140.0);
        QCOMPARE(path.elementAt(path.elementCount()-1).x,140.0);
        drag(w,{140,40},{100,40});
        const auto moved=std::find_if(w.project().nodes.begin(),w.project().nodes.end(),
                                      [&](const Node& node){return node.id==joint;});
        QVERIFY(moved!=w.project().nodes.end());
        QCOMPARE(QPointF(moved->x,moved->y),QPointF(100,140));
        const auto branch_path=branch_item->path();
        const auto branch_end=branch_path.elementAt(branch_path.elementCount()-1);
        const auto branch_before_end=branch_path.elementAt(branch_path.elementCount()-2);
        QCOMPARE(QPointF(branch_end.x,branch_end.y),QPointF(100,140));
        QCOMPARE(branch_before_end.x,100.0);
        std::map<std::string,QPainterPath> paths;
        size_t degree=0;
        for(const auto& wire:w.project().wires) {
            if(wire.from.object!=joint&&wire.to.object!=joint)continue;
            ++degree;
            const auto route=static_cast<QGraphicsPathItem*>(item(w,wire.id))->path();
            QVERIFY(route.elementCount()>=2);
            const auto endpoint=wire.from.object==joint?route.elementAt(0):route.elementAt(route.elementCount()-1);
            QCOMPARE(QPointF(endpoint.x,endpoint.y),QPointF(100,140));
            for(int i=1;i<route.elementCount();++i) {
                const auto previous=route.elementAt(i-1),current=route.elementAt(i);
                QVERIFY(QPointF(previous.x,previous.y)!=QPointF(current.x,current.y));
            }
            paths.emplace(wire.id,route);
        }
        QCOMPARE(degree,size_t(3));
        const auto left_path=paths.at(w.project().wires[0].id);
        const auto right_path=paths.at(w.project().wires[1].id);
        QCOMPARE(left_path.elementAt(left_path.elementCount()-1).x,100.0);
        QCOMPARE(right_path.elementAt(0).x,100.0);
        QCOMPARE(right_path.elementAt(1).x,140.0);
        const auto after=encoded(w.project());
        QVERIFY(after!=before);
        w.undo();QCOMPARE(encoded(w.project()),before);
        w.redo();QCOMPARE(encoded(w.project()),after);
        const auto file=dir.filePath("corner-route.pds");
        QVERIFY(w.save_project(file));
        QVERIFY(w.open_project(file));
        QCOMPARE(encoded(w.project()),after);
        for(const auto& [id,route]:paths)
            QCOMPARE(static_cast<QGraphicsPathItem*>(item(w,id))->path(),route);
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
        field = w.findChild<QLineEdit *>("property_value");
        QVERIFY(field);
        QTest::keyClick(field, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(field, "invalid");
        QTest::keyClick(field, Qt::Key_Return);
        QCOMPARE(w.project().components[0].value, 2000.0);
        w.select_object(b);
        w.select_object(a);
        field = w.findChild<QLineEdit *>("property_value");
        QVERIFY(field);
        QCOMPARE(field->text(), QString("invalid"));
        QTest::keyClick(field, Qt::Key_Escape);
        field = w.findChild<QLineEdit *>("property_value");
        QVERIFY(field);
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
        w.observe_object(w.project().wires.front().id);
        auto *channels = w.findChild<QListWidget *>("channels");
        QVERIFY(channels->count() > 0);
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
        table = w.findChild<QTableWidget *>("property_events");
        QVERIFY(table);
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
int main(int argc, char **argv) { return run_qt_test<InteractionTests>(argc, argv); }
#include "interaction_tests.moc"
