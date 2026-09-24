#include "core/model/hierarchy.hpp"
#include "apps/desktop/editor.hpp"
#include "apps/desktop/number_input.hpp"
#include "apps/desktop/theme.hpp"
#include "core/editor/properties.hpp"
#include "core/model/expression.hpp"
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QHeaderView>
#include <QInputDialog>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <optional>
#include <set>
namespace pds::desktop {
namespace {
constexpr const char *kThreePhaseYDefinition = "1a963f2c-ceb8-5cce-b927-44d735ec9e80";
constexpr const char *kThreePhaseDeltaDefinition = "eb613164-faf4-5b03-9014-806885fef344";
constexpr const char *kThreePhaseVoltageParameter = "6c1aaf47-7a4f-5dac-ab25-cdd71f6816b3";
constexpr const char *kThreePhaseVoltageKindParameter = "9e07a7ea-8295-5fd0-92c6-6e82c8f1c21b";

bool three_phase_source_definition(const std::string &id) {
    return id == kThreePhaseYDefinition || id == kThreePhaseDeltaDefinition;
}
bool three_phase_source_definition(const Definition &definition) {
    if (three_phase_source_definition(definition.id))
        return true;
    const auto name = QString::fromStdString(definition.name).toLower();
    if (name.contains("three-phase voltage source"))
        return true;
    bool has_voltage = false, has_frequency = false;
    for (const auto &parameter : definition.parameters) {
        has_voltage |= parameter.id == kThreePhaseVoltageParameter || parameter.field == "value";
        has_frequency |= parameter.field == "source_frequency";
    }
    return has_voltage && has_frequency && definition.ports.size() >= 4;
}
} // namespace

void EditorWindow::update_instance_specs() {
    bool changed = false;
    std::function<QJsonObject(const Definition &, const PublicParameter &)> binding_field;
    binding_field = [&](const Definition &d, const PublicParameter &p) {
        QJsonObject field;
        for (const auto &child : d.instances)
            if (child.id == p.object) {
                const auto &nested = definition(root_project(), child.definition);
                for (const auto &parameter : nested.parameters)
                    if (parameter.id == p.field)
                        field = binding_field(nested, parameter);
            }
        if (field.isEmpty()) {
            std::string type;
            for (const auto &c : d.components)
                if (c.id == p.object)
                    type = kind_name(c.kind);
            for (const auto &g : d.patterns)
                if (g.id == p.object)
                    type = "pattern";
            if (component_specs_.count(type))
                for (const auto &entry : component_specs_.at(type).value("fields").toArray())
                    if (entry.toObject().value("key").toString() == QString::fromStdString(p.field))
                        field = entry.toObject();
        }
        if (field.isEmpty())
            return field;
        const double scale = field.value("scale").toDouble(1);
        if (p.has_minimum) {
            const double minimum = p.minimum * scale;
            if (!field.contains("min") || minimum > field.value("min").toDouble()) {
                field["min"] = minimum;
                field.remove("exclusiveMin");
            }
        }
        if (p.has_maximum) {
            const double maximum = p.maximum * scale;
            if (!field.contains("max") || maximum < field.value("max").toDouble())
                field["max"] = maximum;
        }
        return field;
    };
    for (const auto &d : root_project().definitions) {
        QJsonArray fields{
            QJsonObject{{"key", "name"}, {"editor", "text"}, {"label", "name"}, {"inline", "name"}}};
        if (three_phase_source_definition(d)) {
            fields.append(QJsonObject{
                {"key", "three_phase_connection"},
                {"editor", "enum"},
                {"label", "three_phase_connection"},
                {"options", QJsonArray{QJsonObject{{"value", 0}, {"label", "three_phase_connection_y"}},
                                       QJsonObject{{"value", 1}, {"label", "three_phase_connection_delta"}}}}});
            fields.append(QJsonObject{
                {"key", "three_phase_voltage_kind"},
                {"editor", "enum"},
                {"label", "three_phase_voltage_kind"},
                {"options", QJsonArray{QJsonObject{{"value", 0}, {"label", "voltage_phase_rms"}},
                                       QJsonObject{{"value", 1}, {"label", "voltage_line_rms"}},
                                       QJsonObject{{"value", 2}, {"label", "voltage_phase_peak"}},
                                       QJsonObject{{"value", 3}, {"label", "voltage_line_peak"}}}}});
            fields.append(QJsonObject{{"key", "three_phase_voltage"},
                                      {"editor", "number"},
                                      {"label", "three_phase_voltage"},
                                      {"unit", "V"},
                                      {"min", 0}});
        }
        for (const auto &p : d.parameters) {
            if (three_phase_source_definition(d) &&
                (p.id == kThreePhaseVoltageParameter || p.id == kThreePhaseVoltageKindParameter))
                continue;
            QJsonObject field = binding_field(d, p);
            field.remove("inline");
            field.remove("inlinePart");
            field.remove("condition");
            // Public parameters are explicit fields of the instance. Conditions
            // of the internal atom reference properties that it alone owns.
            field.remove("when");
            field["key"] = QString::fromStdString("parameter/" + p.id);
            field["editor"] = "number";
            field["displayLabel"] = QString::fromStdString(p.name);
            field["unit"] = QString::fromStdString(p.unit);
            field["group"] = QString::fromStdString(p.group);
            fields.append(field);
        }
        QJsonObject spec{{"fields", fields}};
        auto key = "instance:" + d.id;
        if (!component_specs_.count(key) || component_specs_.at(key) != spec) {
            component_specs_[key] = spec;
            changed = true;
        }
    }
    // The inspector owns a complete editor set per buffered page. A changed
    // dynamic schema is picked up when the next hidden page is built; never
    // append widgets to the currently visible form.
    (void)changed;
}
void EditorWindow::navigate_hierarchy(const std::vector<std::string> &path) {
    if (running() || hierarchy_navigation_)
        return;
    QScopedValueRollback<bool> navigation_guard(hierarchy_navigation_, true);
    try {
        // Locking affects only editing inside a definition. Navigation in both
        // directions is silent and never commits a stray inline editor.
        cancel_inline_edit();
        canvas_->cancel_gesture();
        document_->navigate(path);
        // The root model did not change. Rebuilding the channel catalog here
        // recompiles and flattens the whole project while the native double
        // click is still settling, which can make large locked libraries look
        // like a modal-window loop. Navigation only needs a new scene.
        refresh(false, true);
        canvas_->fitInView(canvas_->scene()->itemsBoundingRect().adjusted(-70, -70, 70, 70),
                           Qt::KeepAspectRatio);
    } catch (const std::exception &e) {
        show_error(e);
    }
}
void EditorWindow::open_subcircuit(const std::string &id) {
    auto path = hierarchy_path();
    path.push_back(id);
    navigate_hierarchy(path);
}
bool EditorWindow::current_hierarchy_locked() const {
    if (hierarchy_path().empty())
        return false;
    const Schematic *level = &root_project();
    for (size_t depth = 0; depth < hierarchy_path().size(); ++depth) {
        const auto &step = hierarchy_path()[depth];
        auto instance = std::find_if(level->instances.begin(), level->instances.end(),
                                     [&](const auto &i) { return i.id == step; });
        if (instance == level->instances.end())
            return true;
        if (depth + 1 == hierarchy_path().size())
            return instance->locked;
        level = &definition(root_project(), instance->definition);
    }
    return true;
}
std::string EditorWindow::group_selection(const QString &name) {
    if (!editing_allowed())
        return {};
    try {
        canvas_->cancel_gesture();
        auto id = document_->create_definition(selected_ids(), name.toStdString());
        refresh();
        select_object(id);
        return id;
    } catch (const std::exception &e) {
        show_error(e);
        return {};
    }
}
void EditorWindow::detach_selected() {
    if (!editing_allowed())
        return;
    try {
        document_->detach_instance(selected_);
        refresh();
    } catch (const std::exception &e) {
        show_error(e);
    }
}
void EditorWindow::expand_selected() {
    if (!editing_allowed())
        return;
    try {
        document_->expand_instance(selected_);
        selected_.clear();
        refresh();
    } catch (const std::exception &e) {
        show_error(e);
    }
}
void EditorWindow::build_hierarchy_actions(QMenu *menu) {
    menu->addSeparator();
    auto add = [&](const char *key, const QKeySequence &shortcut, const std::function<void()> &callback) {
        auto *action = new QAction(text(key), this);
        action->setObjectName(key);
        action->setShortcut(shortcut);
        addAction(action);
        menu->addAction(action);
        commands_[key] = action;
        default_shortcuts_[key] = shortcut;
        connect(action, &QAction::triggered, this, callback);
    };
    add("group_subcircuit", QKeySequence("Ctrl+G"), [this] {
        if (!editing_allowed())
            return;
        bool ok = false;
        auto name = QInputDialog::getText(this, text("group_subcircuit"), text("name"), QLineEdit::Normal,
                                          text("subcircuit"), &ok);
        if (ok && !name.trimmed().isEmpty())
            group_selection(name.trimmed());
    });
    add("open_internals", QKeySequence("Ctrl+Return"), [this] { open_subcircuit(selected_); });
    add("hierarchy_up", QKeySequence("Alt+Up"), [this] {
        auto path = hierarchy_path();
        if (!path.empty()) {
            path.pop_back();
            navigate_hierarchy(path);
        }
    });
    add("edit_definition", {}, [this] {
        if (running())
            return;
        if (std::any_of(project().instances.begin(), project().instances.end(),
                        [&](const auto &i) { return i.id == selected_; }))
            open_subcircuit(selected_);
        if (!hierarchy_path().empty()) {
            hierarchy_edit_enabled_ = !hierarchy_edit_enabled_;
            refresh_hierarchy();
            update_command_state();
            banner_->setText(hierarchy_edit_enabled_ ? text("editing_shared_definition") : text("definition_readonly"));
        }
    });
    commands_.at("edit_definition")->setCheckable(true);
    add("detach_subcircuit", {}, [this] { detach_selected(); });
    add("expand_subcircuit", {}, [this] { expand_selected(); });
    add("public_interface", {}, [this] {
        if (!editing_allowed())
            return;
        for (const auto &i : project().instances)
            if (i.id == selected_) {
                edit_public_interface(i.definition);
                return;
            }
        if (!hierarchy_path().empty())
            edit_public_interface(document_->current_definition());
    });
    add("insert_subcircuit", {}, [this] {
        if (!editing_allowed() || root_project().definitions.empty())
            return;
        QStringList names;
        for (const auto &d : root_project().definitions)
            names.push_back(QString::fromStdString(d.name) + " · " +
                            QString::fromStdString(d.id.substr(0, 8)));
        bool ok = false;
        auto name =
            QInputDialog::getItem(this, text("insert_subcircuit"), text("definition"), names, 0, false, &ok);
        if (ok) {
            try {
                auto index = names.indexOf(name);
                auto point = canvas_->insertion_position();
                auto id = document_->add_instance(root_project().definitions[size_t(index)].id, point.x(),
                                                  point.y());
                refresh();
                select_object(id);
            } catch (const std::exception &e) {
                show_error(e);
            }
        }
    });
}
void EditorWindow::refresh_hierarchy() {
    if (!hierarchy_ || !document_)
        return;
    const auto &root = root_project();
    auto *layout = static_cast<QHBoxLayout *>(breadcrumbs_->layout());
    while (auto *item = layout->takeAt(0)) {
        if (item->widget()) {
            if (item->widget() == definition_button_)
                item->widget()->hide();
            else {
                item->widget()->hide();
                item->widget()->deleteLater();
            }
        }
        delete item;
    }
    auto add_separator = [&] {
        auto *label = new QLabel(">");
        label->setObjectName("hierarchy_separator");
        label->setStyleSheet("color:palette(placeholder-text);padding:0 2px;font-weight:600;");
        layout->addWidget(label);
        label->show();
    };
    auto add_level = [&](const std::string &name, size_t depth) {
        if (depth)
            add_separator();
        auto *button = new QToolButton;
        button->setObjectName("hierarchy_level_" + QString::number(depth));
        const auto title = name.empty() ? text("untitled") : QString::fromStdString(name);
        const int title_width = depth == 0 && hierarchy_path().size() <= 1 ? 440 : 200;
        button->setText(QFontMetrics(button->font()).elidedText(title, Qt::ElideMiddle, title_width));
        button->setToolTip(title);
        button->setCheckable(true);
        button->setChecked(depth == hierarchy_path().size());
        button->setStyleSheet("QToolButton{border:1px solid palette(mid);border-radius:4px;padding:6px 10px;}"
                              "QToolButton:hover,QToolButton:checked{background:palette(alternate-base);}");
        connect(button, &QToolButton::clicked, this, [this, depth] {
            auto path = hierarchy_path();
            if (depth <= path.size()) { path.resize(depth); navigate_hierarchy(path); }
        });
        layout->addWidget(button);
        button->show();
    };
    std::vector<std::string> levels{root.name};
    const Schematic *level = &root;
    size_t depth = 0;
    for (const auto &step : hierarchy_path()) {
        auto i = std::find_if(level->instances.begin(), level->instances.end(),
                              [&](const auto &i) { return i.id == step; });
        if (i == level->instances.end()) break;
        levels.push_back(i->name);
        ++depth;
        level = &definition(root, i->definition);
    }
    if (levels.size() <= 4) {
        for (size_t index = 0; index < levels.size(); ++index)
            add_level(levels[index], index);
    } else {
        add_level(levels.front(), 0);
        add_separator();
        auto *overflow = new QToolButton;
        overflow->setObjectName("hierarchy_overflow");
        overflow->setText(QString::fromUtf8("…"));
        overflow->setToolTip(text("hierarchy"));
        overflow->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(overflow);
        for (size_t index = 1; index + 2 < levels.size(); ++index) {
            const auto title = levels[index].empty() ? text("untitled") : QString::fromStdString(levels[index]);
            auto *action = menu->addAction(title);
            connect(action, &QAction::triggered, this, [this, index] {
                auto path = hierarchy_path();
                if (index <= path.size()) { path.resize(index); navigate_hierarchy(path); }
            });
        }
        overflow->setMenu(menu);
        layout->addWidget(overflow);
        overflow->show();
        for (size_t index = levels.size() - 2; index < levels.size(); ++index)
            add_level(levels[index], index);
    }
    if (!definition_button_) {
        definition_button_ = new QToolButton(breadcrumbs_);
        definition_button_->setObjectName("edit_definition_button");
        definition_button_->setDefaultAction(commands_.at("edit_definition"));
        definition_button_->setToolTip(text("editing_shared_definition"));
    }
    const bool locked = current_hierarchy_locked();
    definition_button_->setVisible(depth != 0 && locked);
    definition_button_->setCheckable(true);
    definition_button_->setChecked(hierarchy_edit_enabled_);
    commands_.at("edit_definition")->setText(text("edit_definition"));
    commands_.at("edit_definition")->setChecked(hierarchy_edit_enabled_);
    if (depth != 0 && locked)
        layout->addWidget(definition_button_);
    layout->addStretch();
    layout->activate();
    std::map<std::string, QString> exposed;
    if (depth)
        for (const auto &port : definition(root, document_->current_definition()).ports)
            exposed[endpoint_key(port.terminal)] += QString::fromStdString(port.name) + " ";
    auto display_port_label = [&](const std::string &object, const QString &, QString label) {
        label = label.trimmed();
        if (label != "p" && label != "n")
            return label;
        auto component = std::find_if(project().components.begin(), project().components.end(),
                                      [&](const auto &c) { return c.id == object; });
        if (component == project().components.end())
            return label;
        if (component->kind == Kind::resistor || component->kind == Kind::inductor ||
            component->kind == Kind::ideal_switch)
            return QString();
        return label == "p" ? QString("+") : QString("-");
    };
    for (auto &[id, atom] : atoms_)
        for (auto *dot : atom->childItems())
            if (dot->data(1).toString() == "port") {
                const auto port = dot->data(2).toString();
                const auto label = display_port_label(
                    id, port, exposed[endpoint_key({id, port.toStdString()})]);
                if (dot->data(7).toString() == label) {
                    for (auto *child : dot->childItems())
                        if (auto *note = dynamic_cast<QGraphicsSimpleTextItem *>(child)) {
                            note->setBrush(theme_colors().signal);
                            note->setTransform(atom->transform().inverted());
                        }
                    continue;
                }
                dot->setData(7, label);
                for (auto *old : dot->childItems())
                    delete old;
                if (!label.isEmpty()) {
                    auto *note = new QGraphicsSimpleTextItem(label, dot);
                    note->setAcceptedMouseButtons(Qt::NoButton);
                    note->setBrush(theme_colors().signal);
                    auto font = note->font();
                    font.setPointSize(8);
                    note->setFont(font);
                    const bool nested = std::any_of(project().instances.begin(), project().instances.end(),
                                                     [&](const auto &instance) { return instance.id == id; });
                    if (nested)
                        note->setPos(dot->pos().x() < 0 ? -note->boundingRect().width() - 6 : 6,
                                     -note->boundingRect().height() / 2);
                    else
                        note->setPos(5, -20);
                    note->setTransform(atom->transform().inverted());
                }
            }
    QStringList structure{QString::fromStdString(root.id), QString::fromStdString(root.name)};
    auto describe = [&](const Schematic &s) {
        for (const auto &i : s.instances)
            structure << QString::fromStdString(i.id) << QString::fromStdString(i.name)
                      << QString::fromStdString(i.definition);
    };
    describe(root);
    for (const auto &d : root.definitions) {
        structure << QString::fromStdString(d.id);
        describe(d);
    }
    const bool rebuild = hierarchy_->property("structure").toStringList() != structure;
    if (rebuild) {
        std::set<QString> expanded;
        for (QTreeWidgetItemIterator it(hierarchy_); *it; ++it)
            if ((*it)->isExpanded())
                expanded.insert((*it)->data(0, Qt::UserRole).toStringList().join('/'));
        hierarchy_->clear();
        auto *root_item = new QTreeWidgetItem(hierarchy_, {QString::fromStdString(root.name)});
        root_item->setData(0, Qt::UserRole, QStringList{});
        root_item->setExpanded(true);
        std::function<void(QTreeWidgetItem *, const Schematic &, QStringList)> children =
            [&](QTreeWidgetItem *parent, const Schematic &schematic, QStringList path) {
                for (const auto &i : schematic.instances) {
                    auto steps = path;
                    steps.push_back(QString::fromStdString(i.id));
                    auto *item = new QTreeWidgetItem(parent, {QString::fromStdString(i.name)});
                    item->setData(0, Qt::UserRole, steps);
                    item->setExpanded(expanded.count(steps.join('/')) != 0);
                    children(item, definition(root, i.definition), steps);
                }
            };
        children(root_item, root, {});
        hierarchy_->setProperty("structure", structure);
    }
    QStringList current_path;
    for (const auto &step : hierarchy_path())
        current_path << QString::fromStdString(step);
    if (rebuild || hierarchy_->property("viewPath").toStringList() != current_path) {
        for (QTreeWidgetItemIterator it(hierarchy_); *it; ++it)
            if ((*it)->data(0, Qt::UserRole).toStringList() == current_path) {
                hierarchy_->setCurrentItem(*it);
                for (auto *parent = (*it)->parent(); parent; parent = parent->parent())
                    parent->setExpanded(true);
                break;
            }
        hierarchy_->setProperty("viewPath", current_path);
    }
    const bool instance = std::any_of(project().instances.begin(), project().instances.end(),
                                      [&](const auto &i) { return i.id == selected_; });
    commands_.at("group_subcircuit")->setEnabled(editing_allowed() && !selected_ids().empty());
    commands_.at("open_internals")->setEnabled(!running() && instance);
    commands_.at("hierarchy_up")->setEnabled(!running() && !hierarchy_path().empty());
    const bool selected_locked = std::any_of(project().instances.begin(), project().instances.end(),
                                             [&](const auto &i) { return i.id == selected_ && i.locked; });
    commands_.at("edit_definition")->setEnabled(!running() && ((instance && selected_locked) || (!hierarchy_path().empty() && locked)));
    commands_.at("detach_subcircuit")->setEnabled(editing_allowed() && instance);
    commands_.at("expand_subcircuit")->setEnabled(editing_allowed() && instance);
    commands_.at("public_interface")
        ->setEnabled(editing_allowed() && (instance || !hierarchy_path().empty()));
    commands_.at("insert_subcircuit")->setEnabled(editing_allowed() && !root.definitions.empty());
    auto *focus = QApplication::focusWidget();
    if (qobject_cast<QLineEdit *>(focus) || qobject_cast<QPlainTextEdit *>(focus) ||
        qobject_cast<QSpinBox *>(focus))
        for (const auto *key : {"group_subcircuit", "open_internals", "hierarchy_up"})
            commands_.at(key)->setEnabled(false);
    canvas_->set_editable(editing_allowed());
    library_->setEnabled(editing_allowed());
}
std::vector<std::string> EditorWindow::visible_plot_channels(const std::string &id) const {
    return plot_channels(root_project(), expanded_uuid(hierarchy_path(), id));
}

void EditorWindow::edit_public_interface(const std::string &definition_id) {
    Definition edited = definition(root_project(), definition_id);
    auto body = definition_project(resolve_parameter_expressions(root_project()), definition_id);
    struct Terminal {
        QString name;
        Endpoint endpoint;
        PortType type;
    };
    std::vector<Terminal> terminals;
    auto terminal = [&](const std::string &id, const std::string &name, const std::string &port,
                        const std::string &label = {}) {
        Endpoint e{id, port};
        terminals.push_back({QString::fromStdString(name + " / " + (label.empty() ? port : label)),
                             e, port_type(body, e)});
    };
    for (const auto &n : body.nodes)
        terminal(n.id, n.name, "node");
    for (const auto &tag : body.tags)
        terminal(tag.id, tag.name, "io");
    for (const auto &c : body.components) {
        terminal(c.id, c.name, "p");
        terminal(c.id, c.name, "n");
        if (gate_controlled(c.kind))
            terminal(c.id, c.name, "gate");
        if (c.kind == Kind::voltage_probe || c.kind == Kind::current_probe)
            terminal(c.id, c.name, "out");
    }
    for (const auto &g : body.patterns)
        for (unsigned output = 0; output < g.outputs; ++output)
            terminal(g.id, g.name, output == 0 ? "out" : "out" + std::to_string(output));
    for (const auto &block : body.code_blocks) {
        for (const auto &input : block.inputs)
            terminal(block.id, block.name, input.id, input.name);
        for (const auto &output : block.outputs)
            terminal(block.id, block.name, output.id, output.name);
    }
    for (const auto &plot : body.plots)
        for (unsigned input = 1; input <= plot.inputs; ++input)
            if (plot.differential) {
                terminal(plot.id, plot.name, "p" + std::to_string(input));
                terminal(plot.id, plot.name, "n" + std::to_string(input));
            } else
                terminal(plot.id, plot.name, "in" + std::to_string(input));
    for (const auto &i : body.instances)
        for (const auto &port : definition(body, i.definition).ports) {
            terminal(i.id, i.name, port.id, port.name);
        }
    struct Binding {
        QString name;
        std::string object, field, unit;
        double value, scale = 1;
        bool has_minimum = false;
        double minimum = 0;
        bool has_maximum = false;
        double maximum = 0;
    };
    struct DefaultValue {
        double value = 0;
        std::string expression;
    };
    std::vector<Binding> bindings;
    auto fields = [&](const auto &objects) {
        for (const auto &object : objects) {
            auto spec = component_specs_.find(object_type(body, object.id));
            if (spec == component_specs_.end())
                continue;
            for (const auto &entry : spec->second.value("fields").toArray()) {
                const auto field = entry.toObject();
                if (field.value("editor").toString() != "number")
                    continue;
                const auto key = field.value("key").toString().toStdString();
                const bool existing_binding = std::any_of(
                    edited.parameters.begin(), edited.parameters.end(),
                    [&](const PublicParameter &parameter) {
                        return parameter.object == object.id && parameter.field == key;
                    });
                if (!existing_binding && !property_visible(body, object.id, field))
                    continue;
                const auto value = read_property(body, object.id, key);
                if (!std::holds_alternative<double>(value))
                    continue;
                const double scale = field.value("scale").toDouble(1);
                bindings.push_back({QString::fromStdString(object.name) + " / " +
                                        text(field.value("label").toString().toUtf8().constData()),
                                    object.id, key, field.value("unit").toString().toStdString(),
                                    std::get<double>(value), scale,
                                    field.contains("min") && !field.value("exclusiveMin").toBool(),
                                    field.value("min").toDouble() / scale,
                                    field.contains("max"), field.value("max").toDouble() / scale});
            }
        }
    };
    fields(body.components);
    fields(body.patterns);
    for (const auto &i : body.instances) {
        const auto &nested = definition(body, i.definition);
        for (const auto &p : nested.parameters) {
            const auto override = std::find_if(i.parameters.begin(), i.parameters.end(),
                                               [&](const auto &entry) { return entry.first == p.id; });
            bindings.push_back({QString::fromStdString(i.name + " / " + p.name), i.id, p.id, p.unit,
                                override == i.parameters.end() ? public_parameter_default_value(nested, p)
                                                               : override->second, 1,
                                p.has_minimum, p.minimum, p.has_maximum, p.maximum});
        }
    }
    // Keep existing bindings that are intentionally broader than a single
    // component field (for example, one mask value for all source phases).
    for (const auto &parameter : edited.parameters) {
        const auto known = std::any_of(bindings.begin(), bindings.end(), [&](const Binding &binding) {
            return binding.object == parameter.object && binding.field == parameter.field;
        });
        if (!known)
            bindings.push_back({QString::fromStdString(parameter.name), parameter.object, parameter.field,
                                parameter.unit, public_parameter_default_value(edited, parameter), 1,
                                parameter.has_minimum, parameter.minimum,
                                parameter.has_maximum, parameter.maximum});
    }
    auto default_value = [&](const QString &input, const Binding &binding,
                             const std::string &parameter) -> DefaultValue {
        try {
            return {parse_si(input.toStdString(), binding.unit) / binding.scale, {}};
        } catch (const std::exception &) {
        }
        const auto source = input.trimmed().toStdString();
        PublicParameter candidate;
        candidate.id = parameter;
        candidate.default_expression = source;
        return {public_parameter_default_value(edited, candidate), source};
    };
    QDialog dialog(this);
    dialog.setObjectName("public_interface_dialog");
    dialog.setWindowTitle(text("public_interface") + " · " + QString::fromStdString(edited.name));
    dialog.resize(1080, 520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *tabs = new QTabWidget;
    layout->addWidget(tabs);
    QTableWidget ports(0, 2), parameters(0, 7);
    ports.setObjectName("public_ports");
    parameters.setObjectName("public_parameters");
    ports.setHorizontalHeaderLabels({text("name"), text("internal_terminal")});
    parameters.setHorizontalHeaderLabels({text("name"), text("parameter_binding"), text("default_value"),
                                          text("parameter_group"), text("unit"), text("minimum"),
                                          text("maximum")});
    auto page = [&](QTableWidget &table, const QString &title, const std::function<void()> &append) {
        auto *widget = new QWidget;
        auto *box = new QVBoxLayout(widget);
        box->addWidget(&table);
        table.horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table.setSelectionBehavior(QAbstractItemView::SelectRows);
        auto *buttons = new QHBoxLayout;
        auto *add = new QPushButton(text("add"));
        add->setObjectName(table.objectName() + "_add");
        auto *remove = new QPushButton(text("delete"));
        remove->setObjectName(table.objectName() + "_delete");
        buttons->addWidget(add);
        buttons->addWidget(remove);
        buttons->addStretch();
        box->addLayout(buttons);
        connect(add, &QPushButton::clicked, &dialog, append);
        connect(remove, &QPushButton::clicked, &dialog, [&table] {
            if (table.currentRow() >= 0)
                table.removeRow(table.currentRow());
        });
        tabs->addTab(widget, title);
    };
    auto add_port = [&](const PublicPort &port) {
        int row = ports.rowCount();
        ports.insertRow(row);
        auto *name = new QTableWidgetItem(QString::fromStdString(port.name));
        name->setData(Qt::UserRole, QString::fromStdString(port.id.empty() ? new_uuid() : port.id));
        ports.setItem(row, 0, name);
        auto *combo = new QComboBox;
        int selected = 0;
        for (size_t n = 0; n < terminals.size(); ++n) {
            combo->addItem(terminals[n].name);
            if (terminals[n].endpoint == port.terminal)
                selected = int(n);
        }
        combo->setCurrentIndex(selected);
        ports.setCellWidget(row, 1, combo);
    };
    auto add_parameter = [&](const PublicParameter &p) {
        int row = parameters.rowCount();
        parameters.insertRow(row);
        auto *name = new QTableWidgetItem(QString::fromStdString(p.name));
        const auto parameter_id = p.id.empty() ? new_uuid() : p.id;
        name->setData(Qt::UserRole, QString::fromStdString(parameter_id));
        parameters.setItem(row, 0, name);
        auto *combo = new QComboBox;
        combo->setObjectName("public_parameter_binding/" + QString::fromStdString(parameter_id));
        int selected = 0;
        for (size_t n = 0; n < bindings.size(); ++n) {
            combo->addItem(bindings[n].name);
            if (bindings[n].object == p.object && bindings[n].field == p.field)
                selected = int(n);
        }
        combo->setCurrentIndex(selected);
        parameters.setCellWidget(row, 1, combo);
        auto *value = new ExpressionLineEdit;
        value->setObjectName("public_parameter_default/" + QString::fromStdString(parameter_id));
        value->setText(p.default_expression.empty()
                           ? QString::number(p.value * bindings.at(size_t(selected)).scale, 'g', 12)
                           : QString::fromStdString(p.default_expression));
        normalize_decimal_point(value);
        parameters.setCellWidget(row, 2, value);
        parameters.setItem(row, 3, new QTableWidgetItem(QString::fromStdString(p.group)));
        auto *unit = new QTableWidgetItem(QString::fromStdString(bindings.at(size_t(selected)).unit));
        unit->setFlags(unit->flags() & ~Qt::ItemIsEditable);
        parameters.setItem(row, 4, unit);
        auto *minimum = new QLineEdit;
        auto *maximum = new QLineEdit;
        minimum->setObjectName("public_parameter_minimum/" + QString::fromStdString(parameter_id));
        maximum->setObjectName("public_parameter_maximum/" + QString::fromStdString(parameter_id));
        normalize_decimal_point(minimum);
        normalize_decimal_point(maximum);
        const auto &selected_binding = bindings.at(size_t(selected));
        if (p.has_minimum || selected_binding.has_minimum)
            minimum->setText(QString::number((p.has_minimum ? p.minimum : selected_binding.minimum) *
                                                 selected_binding.scale,
                                             'g', 12));
        if (p.has_maximum || selected_binding.has_maximum)
            maximum->setText(QString::number((p.has_maximum ? p.maximum : selected_binding.maximum) *
                                                 selected_binding.scale,
                                             'g', 12));
        parameters.setCellWidget(row, 5, minimum);
        parameters.setCellWidget(row, 6, maximum);
        auto update_default = [&, value, combo, parameter_id] {
            try {
                const auto parsed = default_value(value->text(), bindings.at(size_t(combo->currentIndex())),
                                                  parameter_id);
                if (parsed.expression.empty()) {
                    value->set_calculated_value({});
                    value->setToolTip({});
                } else {
                    const auto &binding = bindings.at(size_t(combo->currentIndex()));
                    const auto calculated = engineering_value(parsed.value * binding.scale, binding.unit);
                    value->set_calculated_value(calculated);
                    value->setToolTip(text("parameter_expression").arg(calculated));
                }
            } catch (const std::exception &exception) {
                value->set_calculated_value(QString::fromUtf8("—"));
                value->setToolTip(QString::fromUtf8(exception.what()));
            }
        };
        connect(value, &QLineEdit::textChanged, &dialog, update_default);
        connect(combo, &QComboBox::activated, &dialog, [&, combo, value, minimum, maximum](int n) {
            int row = -1;
            for (int current = 0; current < parameters.rowCount(); ++current)
                if (parameters.cellWidget(current, 1) == combo) {
                    row = current;
                    break;
                }
            if (row < 0)
                return;
            const auto &binding = bindings.at(size_t(n));
            value->setText(QString::number(binding.value * binding.scale, 'g', 12));
            parameters.item(row, 4)->setText(QString::fromStdString(binding.unit));
            minimum->setText(binding.has_minimum
                                 ? QString::number(binding.minimum * binding.scale, 'g', 12)
                                 : QString{});
            maximum->setText(binding.has_maximum
                                 ? QString::number(binding.maximum * binding.scale, 'g', 12)
                                 : QString{});
        });
        update_default();
    };
    auto unused_name = [](const QTableWidget &table, const std::string &stem) {
        for (int number = table.rowCount() + 1;; ++number) {
            const auto candidate = stem + std::to_string(number);
            bool taken = false;
            for (int row = 0; row < table.rowCount(); ++row)
                if (table.item(row, 0)->text().trimmed().toStdString() == candidate) {
                    taken = true;
                    break;
                }
            if (!taken)
                return candidate;
        }
    };
    page(ports, text("public_ports"), [&] {
        if (!terminals.empty())
            add_port({{}, unused_name(ports, "port"), terminals.front().endpoint});
    });
    page(parameters, text("public_parameters"), [&] {
        auto same_field = [](const Binding &a, const Binding &b) {
            return public_parameter_binding_key(a.object, a.field) ==
                   public_parameter_binding_key(b.object, b.field);
        };
        auto available = std::find_if(bindings.begin(), bindings.end(), [&](const Binding &candidate) {
            for (int row = 0; row < parameters.rowCount(); ++row) {
                auto *combo = qobject_cast<QComboBox *>(parameters.cellWidget(row, 1));
                if (combo && same_field(candidate, bindings.at(size_t(combo->currentIndex()))))
                    return false;
            }
            return true;
        });
        if (available != bindings.end()) {
            const auto &b = *available;
            add_parameter({{},
                           unused_name(parameters, "parameter"),
                           b.unit,
                           b.object,
                           b.field,
                           b.value,
                           {},
                           b.has_minimum,
                           b.minimum,
                           b.has_maximum,
                           b.maximum});
        }
    });
    auto *parameter_header = parameters.horizontalHeader();
    parameter_header->setSectionResizeMode(0, QHeaderView::Interactive);
    parameter_header->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int column = 2; column < parameters.columnCount(); ++column)
        parameter_header->setSectionResizeMode(column, QHeaderView::Interactive);
    parameters.setColumnWidth(0, 170);
    parameters.setColumnWidth(2, 125);
    parameters.setColumnWidth(3, 145);
    parameters.setColumnWidth(4, 75);
    parameters.setColumnWidth(5, 110);
    parameters.setColumnWidth(6, 110);
    auto *error = new QLabel;
    error->setObjectName("public_interface_error");
    error->setWordWrap(true);
    auto *appearance_page = new QWidget;
    auto *appearance_layout = new QVBoxLayout(appearance_page);
    auto *symbol = new QComboBox;
    symbol->setObjectName("public_symbol");
    symbol->addItem(text("appearance_automatic"), -1);
    for (const auto &[id, action] : component_actions_) {
        symbol->addItem(component_icon(id), action->text(), id);
        if (edited.appearance.symbol == id)
            symbol->setCurrentIndex(symbol->count() - 1);
    }
    auto *symbol_row = new QHBoxLayout;
    symbol_row->addWidget(new QLabel(text("appearance_symbol")));
    symbol_row->addWidget(symbol, 1);
    appearance_layout->addLayout(symbol_row);
    auto *image_preview = new QLabel;
    image_preview->setObjectName("public_image_preview");
    image_preview->setAlignment(Qt::AlignCenter);
    image_preview->setMinimumHeight(150);
    image_preview->setFrameShape(QFrame::StyledPanel);
    appearance_layout->addWidget(image_preview, 1);
    auto *image_buttons = new QHBoxLayout;
    auto *choose_image = new QPushButton(text("appearance_choose_image"));
    choose_image->setObjectName("public_image_choose");
    auto *clear_image = new QPushButton(text("appearance_clear_image"));
    clear_image->setObjectName("public_image_clear");
    image_buttons->addWidget(choose_image);
    image_buttons->addWidget(clear_image);
    image_buttons->addStretch();
    appearance_layout->addLayout(image_buttons);
    tabs->addTab(appearance_page, text("appearance"));
    QByteArray embedded_image = QByteArray::fromBase64(QByteArray::fromStdString(edited.appearance.image_png));
    auto update_image_preview = [&] {
        QImage image;
        image.loadFromData(embedded_image, "PNG");
        if (image.isNull()) {
            image_preview->setPixmap({});
            image_preview->setText(text("appearance_no_image"));
            clear_image->setEnabled(false);
        } else {
            image_preview->setText({});
            image_preview->setPixmap(QPixmap::fromImage(image).scaled(240, 130, Qt::KeepAspectRatio,
                                                                  Qt::SmoothTransformation));
            clear_image->setEnabled(true);
        }
    };
    connect(choose_image, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getOpenFileName(&dialog, text("appearance_choose_image"), {},
                                                       "Images (*.png *.jpg *.jpeg *.bmp)");
        if (path.isEmpty())
            return;
        QImage image(path);
        if (image.isNull()) {
            error->setText(text("appearance_image_error"));
            return;
        }
        if (image.width() > 512 || image.height() > 512)
            image = image.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG")) {
            error->setText(text("appearance_image_error"));
            return;
        }
        embedded_image = png;
        update_image_preview();
    });
    connect(clear_image, &QPushButton::clicked, &dialog, [&] {
        embedded_image.clear();
        update_image_preview();
    });
    update_image_preview();
    for (const auto &port : edited.ports)
        add_port(port);
    for (const auto &param : edited.parameters)
        add_parameter(param);
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            auto updated = edited;
            updated.ports.clear();
            updated.parameters.clear();
            for (int row = 0; row < ports.rowCount(); ++row) {
                auto *combo = qobject_cast<QComboBox *>(ports.cellWidget(row, 1));
                const auto &t = terminals.at(size_t(combo->currentIndex()));
                PublicPort port{ports.item(row, 0)->data(Qt::UserRole).toString().toStdString(),
                                ports.item(row, 0)->text().trimmed().toStdString(), t.endpoint,
                                t.type.domain, t.type.direction};
                auto previous = std::find_if(edited.ports.begin(), edited.ports.end(),
                                             [&](const auto &old) { return old.id == port.id; });
                if (previous != edited.ports.end()) {
                    port.has_position = previous->has_position;
                    port.x = previous->x;
                    port.y = previous->y;
                }
                updated.ports.push_back(port);
            }
            for (int row = 0; row < parameters.rowCount(); ++row) {
                auto *combo = qobject_cast<QComboBox *>(parameters.cellWidget(row, 1));
                auto b = bindings.at(size_t(combo->currentIndex()));
                const auto parameter_id =
                    parameters.item(row, 0)->data(Qt::UserRole).toString().toStdString();
                auto *value_editor = qobject_cast<QLineEdit *>(parameters.cellWidget(row, 2));
                DefaultValue parsed;
                try {
                    parsed = default_value(value_editor->text(), b, parameter_id);
                } catch (...) {
                    value_editor->setFocus();
                    throw;
                }
                auto limit = [&](int column) -> std::optional<double> {
                    const auto input = qobject_cast<QLineEdit *>(parameters.cellWidget(row, column))
                                           ->text().trimmed();
                    if (input.isEmpty())
                        return {};
                    return parse_si(input.toStdString(), b.unit) / b.scale;
                };
                const auto minimum = limit(5);
                const auto maximum = limit(6);
                PublicParameter parameter;
                parameter.id = parameter_id;
                parameter.name = parameters.item(row, 0)->text().trimmed().toStdString();
                parameter.unit = b.unit;
                parameter.object = b.object;
                parameter.field = b.field;
                parameter.value = parsed.value;
                parameter.group = parameters.item(row, 3)->text().trimmed().toStdString();
                parameter.has_minimum = minimum.has_value();
                parameter.minimum = minimum.value_or(0);
                parameter.has_maximum = maximum.has_value();
                parameter.maximum = maximum.value_or(0);
                parameter.default_expression = std::move(parsed.expression);
                updated.parameters.push_back(std::move(parameter));
            }
            document_->edit_definition(definition_id, [&](Definition &d) {
                d.ports = updated.ports;
                d.parameters = updated.parameters;
                d.appearance.symbol = symbol->currentData().toInt();
                d.appearance.image_png = embedded_image.toBase64().toStdString();
            });
            dialog.accept();
        } catch (const std::exception &e) {
            error->setText(QString::fromUtf8(e.what()));
        }
    });
    if (dialog.exec() == QDialog::Accepted)
        refresh();
}
} // namespace pds::desktop
