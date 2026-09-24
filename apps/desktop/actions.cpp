#include "apps/desktop/theme.hpp"
#include "apps/desktop/editor.hpp"
#include "apps/desktop/code_editor.hpp"
#include "apps/desktop/code_icon_editor.hpp"
#include "apps/desktop/instrumentation.hpp"
#include "apps/desktop/signal_presets.hpp"
#include "apps/desktop/number_input.hpp"
#include "apps/desktop/routing.hpp"
#include "core/model/c_program.hpp"
#include "formats/project/project.hpp"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
namespace pds::desktop {
void EditorWindow::edit_object_icon(const std::string &id) {
    if(!editing_allowed())return;
    std::vector<IconPrimitive> builtin;
    if(auto component=std::find_if(project().components.begin(),project().components.end(),
            [&](const Component &candidate){return candidate.id==id;});component!=project().components.end())
        builtin=editable_component_icon(int(component->kind));
    else if(auto gate=std::find_if(project().patterns.begin(),project().patterns.end(),
            [&](const GatePattern &candidate){return candidate.id==id;});gate!=project().patterns.end())
        builtin={{IconPrimitiveKind::text,IconColor::gate,false,
                  gate->script?"Code":gate->pwm?"PWM":"Gate",{{16,16},{8,0}}}};
    else if(auto plot=std::find_if(project().plots.begin(),project().plots.end(),
            [&](const PlotBlock &candidate){return candidate.id==id;});plot!=project().plots.end())
        builtin=editable_component_icon(plot->differential?107:103);
    else if(auto tag=std::find_if(project().tags.begin(),project().tags.end(),
            [&](const ConnectionTag &candidate){return candidate.id==id;});tag!=project().tags.end())
        builtin={{IconPrimitiveKind::text,tag->domain==Domain::gate?IconColor::gate:IconColor::foreground,
                  false,tag->domain==Domain::gate?"G":"N",{{16,16},{10,0}}}};
    else if(auto instance=std::find_if(project().instances.begin(),project().instances.end(),
            [&](const Instance &candidate){return candidate.id==id;});instance!=project().instances.end()) {
        const auto definition_it=std::find_if(project().definitions.begin(),project().definitions.end(),
            [&](const Definition &candidate){return candidate.id==instance->definition;});
        if(definition_it!=project().definitions.end()) {
            const int symbol=definition_it->appearance.symbol>=0?definition_it->appearance.symbol:
                             definition_icon_id(definition_it->id);
            builtin=editable_component_icon(symbol);
        }
    }
    if(builtin.empty())builtin=default_code_icon(108);
    std::vector<IconPrimitive> original=builtin;
    const auto block=std::find_if(project().code_blocks.begin(),project().code_blocks.end(),
                                  [&](const CodeBlock &candidate){return candidate.id==id;});
    if(block!=project().code_blocks.end())original=block->icon.empty()?default_code_icon(108):block->icon;
    else if(auto appearance=std::find_if(project().object_icons.begin(),project().object_icons.end(),
                    [&](const ObjectIcon &candidate){return candidate.object==id;});appearance!=project().object_icons.end())
        original=appearance->primitives;
    QDialog dialog(this);dialog.setObjectName("object_icon_dialog");dialog.setWindowTitle(text("edit_object_icon"));dialog.resize(760,620);
    auto *layout=new QVBoxLayout(&dialog);
    auto *editor=new CodeIconEditor(original,&dialog);layout->addWidget(editor,1);
    auto *hint=new QLabel(text("object_icon_hint"));hint->setWordWrap(true);layout->addWidget(hint);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);
    auto *standard=buttons->addButton(text("restore_standard_icon"),QDialogButtonBox::ResetRole);
    standard->setObjectName("restore_standard_icon");layout->addWidget(buttons);
    connect(standard,&QPushButton::clicked,&dialog,[editor,builtin]{editor->set_icon(builtin);});
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if(dialog.exec()!=QDialog::Accepted)return;
    const auto edited=editor->icon();
    const auto icon=edited==builtin?std::vector<IconPrimitive>{}:edited;
    document_->apply("Edit object icon",[&](Project &p) {
        if(auto target=std::find_if(p.code_blocks.begin(),p.code_blocks.end(),
                [&](const CodeBlock &candidate){return candidate.id==id;});target!=p.code_blocks.end()) {
            target->icon=icon;return;
        }
        std::erase_if(p.object_icons,[&](const ObjectIcon &appearance){return appearance.object==id;});
        if(!icon.empty())p.object_icons.push_back({id,icon});
    });
    refresh_canvas(false,false);
}

void EditorWindow::edit_code_block(const std::string &id) {
    if (!editing_allowed())
        return;
    const auto found = std::find_if(project().code_blocks.begin(), project().code_blocks.end(),
                                    [&](const CodeBlock &block) { return block.id == id; });
    if (found == project().code_blocks.end())
        return;
    const CodeBlock original = *found;
    QDialog dialog(this);
    dialog.setObjectName("code_block_dialog");
    dialog.setWindowTitle(text("edit_code_block"));
    dialog.setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    dialog.setWindowFlag(Qt::WindowMinimizeButtonHint, true);
    dialog.resize(1000, 760);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *name = new QLineEdit(QString::fromStdString(original.name));
    auto *period = new QLineEdit(engineering_value(original.period, "s"));
    auto *phase = new QLineEdit(engineering_value(original.phase, "s"));
    name->setObjectName("code_block_name");
    period->setObjectName("code_block_period");
    phase->setObjectName("code_block_phase");
    normalize_decimal_point(period);
    normalize_decimal_point(phase);
    form->addRow(text("name"), name);
    form->addRow(text("code_block_period"), period);
    form->addRow(text("code_block_phase"), phase);
    layout->addLayout(form);

    auto *tabs = new QTabWidget;
    tabs->setObjectName("code_block_tabs");
    auto *editor = new CCodeEdit(true, &dialog);
    editor->setObjectName("code_block_code");
    editor->setPlainText(QString::fromStdString(original.code));
    tabs->addTab(editor, text("code_block_code_tab"));
    struct PortTable { QTableWidget *table = nullptr; bool input = false; };
    auto make_table = [&](const std::vector<CodePort> &ports, bool input) {
        auto *page = new QWidget;
        auto *page_layout = new QVBoxLayout(page);
        auto *table = new QTableWidget(0, 4);
        table->setObjectName(input ? "code_block_inputs" : "code_block_outputs");
        table->setHorizontalHeaderLabels({text("name"), text("code_block_type"),
                                          text("code_block_unit"), text("initial_value")});
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
        table->verticalHeader()->setVisible(false);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        auto insert = [table](int row, const CodePort &port) {
            table->insertRow(row);
            auto *name_item = new QTableWidgetItem(QString::fromStdString(port.name));
            name_item->setData(Qt::UserRole, QString::fromStdString(port.id));
            table->setItem(row, 0, name_item);
            auto *type = new QComboBox;
            type->addItem("double", int(SignalScalarType::real));
            type->addItem("bool", int(SignalScalarType::boolean));
            type->setCurrentIndex(port.type == SignalScalarType::boolean ? 1 : 0);
            table->setCellWidget(row, 1, type);
            table->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(port.unit)));
            table->setItem(row, 3, new QTableWidgetItem(QString::number(port.initial, 'g', 15)));
        };
        for (const auto &port : ports)
            insert(table->rowCount(), port);
        auto *buttons = new QHBoxLayout;
        auto *add = new QPushButton(text("add"));
        auto *remove = new QPushButton(text("delete"));
        auto *up = new QPushButton(text("move_up"));
        auto *down = new QPushButton(text("move_down"));
        add->setObjectName(input ? "add_code_input" : "add_code_output");
        remove->setObjectName(input ? "remove_code_input" : "remove_code_output");
        up->setObjectName(input ? "move_code_input_up" : "move_code_output_up");
        down->setObjectName(input ? "move_code_input_down" : "move_code_output_down");
        buttons->addWidget(add); buttons->addWidget(remove); buttons->addWidget(up); buttons->addWidget(down);
        buttons->addStretch();
        page_layout->addWidget(table);
        page_layout->addLayout(buttons);
        connect(add, &QPushButton::clicked, &dialog, [=] {
            const QString stem = input ? "input" : "output";
            int number = table->rowCount() + 1;
            auto exists = [&](const QString &candidate) {
                for (int row = 0; row < table->rowCount(); ++row)
                    if (table->item(row, 0) && table->item(row, 0)->text() == candidate) return true;
                return false;
            };
            while (exists(stem + QString::number(number))) ++number;
            insert(table->rowCount(), {new_uuid(), (stem + QString::number(number)).toStdString(), "",
                                       SignalScalarType::real, 0});
            table->selectRow(table->rowCount() - 1);
        });
        connect(remove, &QPushButton::clicked, &dialog, [table] {
            if (table->currentRow() >= 0) table->removeRow(table->currentRow());
        });
        auto swap_rows = [table](int from, int to) {
            if (from < 0 || to < 0 || from >= table->rowCount() || to >= table->rowCount()) return;
            struct Row { QString name, id, unit, initial; int type = 0; } rows[2];
            for (int i = 0; i < 2; ++i) {
                const int row = i ? to : from;
                rows[i].name = table->item(row, 0) ? table->item(row, 0)->text() : QString();
                rows[i].id = table->item(row, 0) ? table->item(row, 0)->data(Qt::UserRole).toString() : QString();
                rows[i].unit = table->item(row, 2) ? table->item(row, 2)->text() : QString();
                rows[i].initial = table->item(row, 3) ? table->item(row, 3)->text() : QString();
                if (auto *combo = qobject_cast<QComboBox *>(table->cellWidget(row, 1))) rows[i].type = combo->currentData().toInt();
            }
            for (int i = 0; i < 2; ++i) {
                const int row = i ? from : to;
                auto *name_item = new QTableWidgetItem(rows[i].name);
                name_item->setData(Qt::UserRole, rows[i].id);
                table->setItem(row, 0, name_item);
                auto *type = new QComboBox;
                type->addItem("double", int(SignalScalarType::real));
                type->addItem("bool", int(SignalScalarType::boolean));
                type->setCurrentIndex(type->findData(rows[i].type));
                table->setCellWidget(row, 1, type);
                table->setItem(row, 2, new QTableWidgetItem(rows[i].unit));
                table->setItem(row, 3, new QTableWidgetItem(rows[i].initial));
            }
            table->selectRow(to);
        };
        connect(up, &QPushButton::clicked, &dialog, [=] { swap_rows(table->currentRow(), table->currentRow() - 1); });
        connect(down, &QPushButton::clicked, &dialog, [=] { swap_rows(table->currentRow(), table->currentRow() + 1); });
        tabs->addTab(page, text(input ? "code_block_inputs" : "code_block_outputs"));
        return PortTable{table, input};
    };
    const auto inputs = make_table(original.inputs, true);
    const auto outputs = make_table(original.outputs, false);
    auto *icon_editor = new CodeIconEditor(original.icon.empty() ? default_code_icon(108) : original.icon);
    tabs->addTab(icon_editor, text("code_block_icon"));
    layout->addWidget(tabs, 1);
    auto *status = new QLabel;
    status->setObjectName("code_block_status");
    status->setWordWrap(true);
    layout->addWidget(status);
    auto *bottom = new QHBoxLayout;
    auto *format = new QPushButton(text("format_code"));
    auto *compile_button = new QPushButton(text("compile_code"));
    auto *help = new QPushButton(text("functions_help"));
    auto *maximize = new QPushButton(text("maximize_window"));
    format->setObjectName("format_code_block");
    compile_button->setObjectName("compile_code_block");
    help->setObjectName("code_block_functions_help");
    maximize->setObjectName("maximize_code_block_dialog");
    bottom->addWidget(format); bottom->addWidget(compile_button); bottom->addWidget(help); bottom->addWidget(maximize); bottom->addStretch();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bottom->addWidget(buttons);
    layout->addLayout(bottom);
    connect(format, &QPushButton::clicked, editor, &CCodeEdit::format_code);
    connect(help,&QPushButton::clicked,&dialog,[&dialog]{show_c_code_reference(&dialog);});
    connect(maximize,&QPushButton::clicked,&dialog,[&dialog,maximize] {
        if(dialog.isMaximized()||dialog.isFullScreen()) {
            dialog.showNormal();
            maximize->setText(text("maximize_window"));
        } else {
            dialog.showMaximized();
            maximize->setText(text("restore_window"));
        }
    });

    auto read_ports = [&](const PortTable &source) {
        std::vector<CodePort> result;
        for (int row = 0; row < source.table->rowCount(); ++row) {
            const auto *name_item = source.table->item(row, 0);
            const auto *unit_item = source.table->item(row, 2);
            const auto *initial_item = source.table->item(row, 3);
            auto *type = qobject_cast<QComboBox *>(source.table->cellWidget(row, 1));
            CodePort port;
            port.id = name_item ? name_item->data(Qt::UserRole).toString().toStdString() : std::string();
            if (port.id.empty()) port.id = new_uuid();
            port.name = name_item ? name_item->text().trimmed().toStdString() : std::string();
            port.unit = unit_item ? unit_item->text().trimmed().toStdString() : std::string();
            port.type = type && type->currentData().toInt() == int(SignalScalarType::boolean)
                            ? SignalScalarType::boolean : SignalScalarType::real;
            port.initial = parse_si(initial_item ? initial_item->text().toStdString() : std::string(), port.unit);
            result.push_back(std::move(port));
        }
        return result;
    };
    auto build = [&](bool compile_source = true) {
        CodeBlock block = original;
        block.name = name->text().trimmed().toStdString();
        block.period = parse_si(period->text().toStdString(), "s");
        block.phase = parse_si(phase->text().toStdString(), "s");
        block.code = editor->toPlainText().toStdString();
        block.icon = icon_editor->icon();
        block.inputs = read_ports(inputs);
        block.outputs = read_ports(outputs);
        if (block.name.empty()) throw std::runtime_error(text("code_block_name_required").toStdString());
        if (!std::isfinite(block.period) || block.period <= 0 || !std::isfinite(block.phase) || block.phase < 0)
            throw std::runtime_error(text("code_block_schedule_error").toStdString());
        if (block.code.empty() || block.outputs.empty())
            throw std::runtime_error(text("code_block_output_required").toStdString());
        QRegularExpression identifier("^[A-Za-z_][A-Za-z0-9_]*$");
        std::set<std::string> names{"t", "stime", "dt"};
        std::set<std::string> ids;
        auto validate = [&](const CodePort &port) {
            if (!valid_uuid(port.id) || !ids.insert(port.id).second ||
                !identifier.match(QString::fromStdString(port.name)).hasMatch() || !names.insert(port.name).second)
                throw std::runtime_error(text("code_block_port_error").arg(QString::fromStdString(port.name)).toStdString());
            if (!std::isfinite(port.initial) ||
                (port.type == SignalScalarType::boolean && port.initial != 0 && port.initial != 1))
                throw std::runtime_error(text("code_block_initial_error").arg(QString::fromStdString(port.name)).toStdString());
        };
        for (const auto &port : block.inputs) validate(port);
        for (const auto &port : block.outputs) validate(port);
        if (compile_source) {
            CProgramOptions options;
            options.diagnostic_code = "invalid_code_block";
            options.object = block.id;
            options.allow_time = true;
            options.external_variables.insert("dt");
            for (const auto &port : block.inputs) options.external_variables.insert(port.name);
            for (const auto &port : block.outputs) {
                options.external_variables.insert(port.name);
                options.writable_variables.insert(port.name);
            }
            (void)compile_c_program(block.code, options);
        }
        return block;
    };
    connect(compile_button, &QPushButton::clicked, &dialog, [&] {
        try { (void)build(); status->setText(text("code_valid")); }
        catch (const std::exception &error) { status->setText(QString::fromUtf8(error.what())); }
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            const auto block = build(false);
            std::set<std::string> retained;
            for (const auto &port : block.inputs) retained.insert(port.id);
            for (const auto &port : block.outputs) retained.insert(port.id);
            for (const auto &wire : project().wires) {
                auto removed = [&](const Endpoint &endpoint) {
                    return endpoint.object == id && !retained.contains(endpoint.port);
                };
                if (removed(wire.from) || removed(wire.to)) {
                    const auto &endpoint = removed(wire.from) ? wire.from : wire.to;
                    std::string port_name = endpoint.port;
                    auto resolve_name = [&](const std::vector<CodePort> &ports) {
                        if (auto port = std::find_if(ports.begin(), ports.end(),
                                                     [&](const CodePort &candidate) { return candidate.id == endpoint.port; });
                            port != ports.end())
                            port_name = port->name;
                    };
                    resolve_name(original.inputs);
                    resolve_name(original.outputs);
                    throw Diagnostic("connected_code_port", id,
                                     text("code_block_connected_port").arg(QString::fromStdString(port_name)).toStdString());
                }
            }
            (void)build(true);
            document_->apply("Edit code block", [&](Project &p) {
                auto target = std::find_if(p.code_blocks.begin(), p.code_blocks.end(),
                                           [&](const CodeBlock &candidate) { return candidate.id == id; });
                if (target == p.code_blocks.end()) throw std::runtime_error("Code block no longer exists");
                *target = block;
            });
            dialog.accept();
        } catch (const std::exception &error) {
            status->setText(QString::fromUtf8(error.what()));
        }
    });
    if (dialog.exec() == QDialog::Accepted)
        refresh();
}

void EditorWindow::show_command_search() {
    QDialog dialog(this);
    dialog.setObjectName("command_search_dialog");
    dialog.setWindowTitle(text("command_search"));
    dialog.resize(640, 440);
    auto *layout = new QVBoxLayout(&dialog);
    auto *input = new QLineEdit;
    input->setObjectName("command_search_input");
    input->setPlaceholderText(text("command_search_hint"));
    auto *list = new QListWidget;
    list->setObjectName("command_search_results");
    layout->addWidget(input);
    layout->addWidget(list);
    std::vector<QPointer<QAction>> actions;
    std::set<QAction *> added;
    std::function<void(const QList<QAction *> &, const QString &)> collect = [&](const auto &entries, const QString &prefix) {
        for (auto *action : entries) {
            if (action->isSeparator()) continue;
            auto name = action->text(); name.remove('&');
            const auto label = prefix + name;
            if (action->menu()) { collect(action->menu()->actions(), label + " → "); continue; }
            if (!added.insert(action).second) continue;
            auto *item = new QListWidgetItem(label + (action->shortcut().isEmpty() ? QString() : "    " + action->shortcut().toString(QKeySequence::NativeText)), list);
            item->setData(Qt::UserRole, int(actions.size()));
            item->setToolTip(action->toolTip());
            if (!action->isEnabled()) item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            actions.push_back(action);
        }
    };
    collect(menuBar()->actions(), {});
    for (const auto &[id, action] : component_actions_) collect({action}, text("library") + " → ");
    QPointer<QAction> selected;
    auto choose = [&] {
        auto *item = list->currentItem();
        if (!item || item->isHidden() || !(item->flags() & Qt::ItemIsEnabled)) return;
        selected = actions.at(size_t(item->data(Qt::UserRole).toInt()));
        dialog.accept();
    };
    connect(input, &QLineEdit::textChanged, &dialog, [&](const QString &query) {
        QListWidgetItem *first = nullptr;
        for (int i = 0; i < list->count(); ++i) {
            auto *item = list->item(i);
            bool matches = true;
            for (const auto &word : query.simplified().split(' ', Qt::SkipEmptyParts))
                matches &= (item->text() + " " + item->toolTip()).contains(word, Qt::CaseInsensitive);
            item->setHidden(!matches);
            if (matches && !first && (item->flags() & Qt::ItemIsEnabled)) first = item;
        }
        list->setCurrentItem(first);
    });
    connect(input, &QLineEdit::returnPressed, &dialog, choose);
    connect(list, &QListWidget::itemActivated, &dialog, [&](QListWidgetItem *) { choose(); });
    list->setCurrentRow(0);
    input->setFocus();
    if (dialog.exec() == QDialog::Accepted && selected && selected->isEnabled()) selected->trigger();
}
std::vector<std::string> EditorWindow::selected_ids() const {
    std::vector<std::string> ids;
    for (auto *item : canvas_->scene()->selectedItems()) {
        if (item->data(1).toString() == "label")
            continue;
        const auto kind = item->data(1).toString();
        if (kind != "atom" && kind != "wire")
            continue;
        auto id = item->data(0).toString().toStdString();
        if (!id.empty())
            ids.push_back(id);
    }
    return ids;
}
void EditorWindow::transform_selection(int turns, bool mirror) {
    if (running())
        return;
    if (paste_fragment_) {
        auto turn = [&](double &x, double &y) {
            if (mirror)
                x = -x;
            else {
                int n = (turns % 4 + 4) % 4;
                while (n--) {
                    double old = x;
                    x = -y;
                    y = old;
                }
            }
        };
        auto transform = [&](auto &list) {
            for (auto &o : list) {
                turn(o.x, o.y);
                if (mirror)
                    o.orientation.mirrored = !o.orientation.mirrored;
                else
                    o.orientation.quarter_turns =
                        (o.orientation.quarter_turns + (o.orientation.mirrored ? -turns : turns) + 4) % 4;
            }
        };
        transform(paste_fragment_->components);
        transform(paste_fragment_->nodes);
        transform(paste_fragment_->tags);
        transform(paste_fragment_->patterns);
        transform(paste_fragment_->plots);
        transform(paste_fragment_->code_blocks);
        transform(paste_fragment_->instances);
        for (auto &w : paste_fragment_->wires)
            for (auto &p : w.bends)
                turn(p.x, p.y);
        set_placement_preview();
        return;
    }
    if (canvas_->transform_move(turns, mirror) || canvas_->editing_gesture())
        return;
    if (transform_labels(turns, mirror))
        return;
    auto ids = selected_ids();
    if (ids.empty())
        return;
    try {
        document_->transform(ids, turns, mirror);
        refresh();
        for (const auto &id : ids)
            if (atoms_.count(id))
                atoms_.at(id)->setSelected(true);
    } catch (const std::exception &e) {
        show_error(e);
    }
}
void EditorWindow::scale_selection(double factor) {
    if (running())
        return;
    auto apply_scale = [&](auto &list, const std::set<std::string> &ids) {
        for (auto &object : list)
            if (ids.count(object.id)) {
                const double next = factor == 0 ? 1.0 : std::clamp(object.orientation.scale * factor, 0.5, 2.5);
                object.orientation.scale = std::round(next * 10.0) / 10.0;
            }
    };
    if (paste_fragment_) {
        std::set<std::string> ids;
        auto collect = [&](const auto &list) {
            for (const auto &object : list)
                ids.insert(object.id);
        };
        collect(paste_fragment_->components);
        collect(paste_fragment_->nodes);
        collect(paste_fragment_->tags);
        collect(paste_fragment_->patterns);
        collect(paste_fragment_->plots);
        collect(paste_fragment_->code_blocks);
        collect(paste_fragment_->instances);
        apply_scale(paste_fragment_->components, ids);
        apply_scale(paste_fragment_->nodes, ids);
        apply_scale(paste_fragment_->tags, ids);
        apply_scale(paste_fragment_->patterns, ids);
        apply_scale(paste_fragment_->plots, ids);
        apply_scale(paste_fragment_->code_blocks, ids);
        apply_scale(paste_fragment_->instances, ids);
        set_placement_preview();
        return;
    }
    if (canvas_->editing_gesture())
        return;
    auto list = selected_ids();
    std::set<std::string> ids(list.begin(), list.end());
    if (ids.empty())
        return;
    try {
        document_->apply("Scale objects", [&](Project &p) {
            apply_scale(p.components, ids);
            apply_scale(p.nodes, ids);
            apply_scale(p.tags, ids);
            apply_scale(p.patterns, ids);
            apply_scale(p.plots, ids);
            apply_scale(p.code_blocks, ids);
            apply_scale(p.instances, ids);
        });
        refresh();
        for (const auto &id : list)
            if (atoms_.count(id))
                atoms_.at(id)->setSelected(true);
    } catch (const std::exception &e) {
        show_error(e);
    }
}
void EditorWindow::arrange_selection(const std::string &mode) {
    if (running())
        return;
    canvas_->cancel_gesture();
    auto ids = selected_ids();
    if (ids.size() < 2)
        return;
    try {
        document_->arrange(ids, mode);
        refresh();
        for (const auto &id : ids)
            if (atoms_.count(id))
                atoms_.at(id)->setSelected(true);
    } catch (const std::exception &e) {
        show_error(e);
    }
}
Project EditorWindow::copy_visible_selection(const std::vector<std::string>& ids) const {
    auto fragment = document_->copy(ids);
    const std::set<std::string> selected(ids.begin(),ids.end());
    for(const auto& wire:project().wires) {
        const auto view=hidden_current_wire_view(project(),wire.id);
        if(!view||wire.id!=view->primary||!selected.count(view->from.object)||!selected.count(view->to.object))
            continue;
        auto visible=wire;
        visible.from=view->from;
        visible.to=view->to;
        visible.bends=view->bends;
        fragment.wires.push_back(std::move(visible));
    }
    return fragment;
}
bool EditorWindow::copy_selection(bool cut) {
    if (cut && running())
        return false;
    if (cut)
        canvas_->cancel_gesture();
    auto ids = selected_ids();
    if (ids.empty())
        return false;
    auto fragment = copy_visible_selection(ids);
    if (fragment.components.empty() && fragment.nodes.empty() && fragment.tags.empty() && fragment.patterns.empty() &&
        fragment.plots.empty() && fragment.code_blocks.empty() && fragment.instances.empty())
        return false;
    try {
        std::ostringstream out;
        write_project(fragment, out);
        auto *mime = new QMimeData;
        auto bytes = QByteArray::fromStdString(out.str());
        mime->setData("application/x-powerdrivesim-project", bytes);
        mime->setText(QString::fromUtf8(bytes));
        QApplication::clipboard()->setMimeData(mime);
        if (cut) {
            erase_with_hidden_current_probes(ids);
            selected_.clear();
            refresh();
        }
        return true;
    } catch (const std::exception &e) {
        show_error(e);
        return false;
    }
}
void EditorWindow::paste_selection(bool duplicate) {
    if (running())
        return;
    try {
        Project fragment;
        if (duplicate)
            fragment = copy_visible_selection(selected_ids());
        else {
            const auto *mime = QApplication::clipboard()->mimeData();
            if (!mime->hasFormat("application/x-powerdrivesim-project"))
                return;
            std::istringstream in(mime->data("application/x-powerdrivesim-project").toStdString());
            fragment = read_project(in);
        }
        if (fragment.components.empty() && fragment.nodes.empty() && fragment.tags.empty() && fragment.patterns.empty() &&
            fragment.plots.empty() && fragment.code_blocks.empty() && fragment.instances.empty())
            return;
        canvas_->cancel_gesture();
        double x = 0, y = 0;
        size_t count = 0;
        auto center = [&](const auto &list) {
            for (const auto &o : list) {
                x += o.x;
                y += o.y;
                ++count;
            }
        };
        center(fragment.components);
        center(fragment.nodes);
        center(fragment.tags);
        center(fragment.patterns);
        center(fragment.plots);
        center(fragment.code_blocks);
        center(fragment.instances);
        x = count ? std::round(x / count / 20) * 20 : 0;
        y = count ? std::round(y / count / 20) * 20 : 0;
        auto shift = [&](auto &list) {
            for (auto &o : list) {
                o.x -= x;
                o.y -= y;
            }
        };
        shift(fragment.components);
        shift(fragment.nodes);
        shift(fragment.tags);
        shift(fragment.patterns);
        shift(fragment.plots);
        shift(fragment.code_blocks);
        shift(fragment.instances);
        for (auto &w : fragment.wires)
            for (auto &p : w.bends) {
                p.x -= x;
                p.y -= y;
            }
        paste_fragment_ = std::move(fragment);
        set_placement_preview();
        canvas_->setFocus();
    } catch (const std::exception &e) {
        show_error(e);
    }
}
void EditorWindow::show_context(const std::string &id, QPoint global) {
    QGraphicsItem *label = nullptr;
    for (auto *candidate : canvas_->items(canvas_->viewport()->mapFromGlobal(global)))
        if (candidate->data(1).toString() == "label") {
            label = candidate;
            break;
        }
    if (label) {
        canvas_->scene()->clearSelection();
        label->setSelected(true);
        const auto role = label->data(2).toString().toStdString();
        QMenu menu(this);
        for (const char *key : {"rotate", "rotate_back", "mirror"})
            menu.addAction(commands_.at(key));
        menu.addAction(text("reset_label"), this, [this, id, role] {
            document_->apply("Reset label", [&](Project &p) {
                std::erase_if(p.labels,
                              [&](const LabelLayout &l) { return l.object == id && l.role == role; });
            });
            refresh(false);
        });
        menu.addSeparator();
        menu.addAction(text("select_block"), this, [this, id] { select_object(id); });
        menu.exec(global);
        return;
    }
    if (std::any_of(project().nodes.begin(), project().nodes.end(),
                    [&](const Node &node) { return node.id == id; }))
        select_object(id);
    if (!id.empty()) {
        auto *item = atoms_.count(id) ? atoms_.at(id) : (wires_.count(id) ? wires_.at(id) : nullptr);
        if (item && !item->isSelected())
            select_object(id);
    }
    std::vector<std::string> selected_wire_ids;
    for (const auto &selected : selected_ids())
        if (std::any_of(project().wires.begin(), project().wires.end(),
                        [&](const Wire &value) { return value.id == selected; }))
            selected_wire_ids.push_back(selected);
    QMenu menu(this);
    menu.setObjectName("element_context");
    if (!id.empty()) {
        menu.addAction(commands_.at("properties"));
        if (std::any_of(project().instances.begin(), project().instances.end(),
                        [&](const auto &i) { return i.id == id; })) {
            for (const char *key : {"open_internals", "edit_definition", "detach_subcircuit",
                                    "expand_subcircuit", "public_interface"})
                menu.addAction(commands_.at(key));
            auto *lock = menu.addAction(text("lock_subcircuit"));
            lock->setCheckable(true);
            lock->setChecked(std::any_of(project().instances.begin(), project().instances.end(),
                                         [&](const auto &i) { return i.id == id && i.locked; }));
            connect(lock, &QAction::triggered, this, [this, id](bool checked) {
                document_->apply("Lock subcircuit", [&](Project &p) {
                    for (auto &i : p.instances)
                        if (i.id == id)
                            i.locked = checked;
                });
                refresh();
            });
        }
        if (std::any_of(project().plots.begin(), project().plots.end(),
                        [&](const PlotBlock &g) { return g.id == id; }))
            menu.addAction(text("plot"), this, [this, id] { open_plot(id); });
        auto *arrange = menu.addMenu(text("arrange"));
        for (const char *key :
             {"rotate", "rotate_back", "mirror", "scale_up", "scale_down", "scale_reset", "left", "right", "top",
              "bottom", "horizontal", "vertical"})
            arrange->addAction(commands_.at(key));
        menu.addSeparator();
        for (const char *key : {"copy", "cut", "duplicate", "delete"})
            menu.addAction(commands_.at(key));
        const bool wire = std::any_of(project().wires.begin(), project().wires.end(),
                                      [&](const Wire &value) { return value.id == id; });
        const bool component = std::any_of(project().components.begin(), project().components.end(),
                                           [&](const Component &value) { return value.id == id; });
        const bool component_current = std::any_of(project().components.begin(), project().components.end(),
                                                   [&](const Component &value) {
                                                       return value.id == id && value.kind != Kind::voltage_probe;
                                                   });
        const bool instance = std::any_of(project().instances.begin(), project().instances.end(),
                                          [&](const Instance &value) { return value.id == id; });
        if (wire) {
            auto wire_ids = selected_wire_ids;
            if (std::find(wire_ids.begin(), wire_ids.end(), id) == wire_ids.end())
                wire_ids.push_back(id);
            auto *observe = menu.addMenu(text("observe"));
            observe->addAction(text("observe_voltage"), this,
                               [this, wire_ids] { observe_wires(wire_ids, false); });
            observe->addAction(text("observe_wire_current"), this,
                               [this, wire_ids] { observe_wires(wire_ids, true); });
        } else if (component || instance) {
            auto *observe = menu.addMenu(text("observe"));
            observe->addAction(text("observe_terminal_voltages"), this,
                               [this, id] { observe_component_terminals(id); });
            if (component_current)
                observe->addAction(text("observe_current"), this, [this, id] { observe_object(id); });
        }
    }
    if (id.empty() && !selected_wire_ids.empty()) {
        auto *observe = menu.addMenu(text("observe"));
        observe->addAction(text("observe_voltage"), this,
                           [this, selected_wire_ids] { observe_wires(selected_wire_ids, false); });
        observe->addAction(text("observe_wire_current"), this,
                           [this, selected_wire_ids] { observe_wires(selected_wire_ids, true); });
        menu.addSeparator();
    }
    menu.addAction(commands_.at("paste"));
    menu.addAction(commands_.at("group_subcircuit"));
    menu.addSeparator();
    menu.addAction(commands_.at("shortcuts"));
    menu.exec(global);
}
void EditorWindow::load_shortcuts() {
    QSettings settings(recovery_dir_ + "/shortcuts.ini", QSettings::IniFormat);
    const int allowed = Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier;
    auto modifier = [&](const char *key, int fallback) {
        int v = settings.value(key, fallback).toInt();
        return Qt::KeyboardModifiers(v > 0 && (v & ~allowed) == 0 ? v : fallback);
    };
    scope_wheel_x_ = modifier("scope/wheel_x", Qt::ControlModifier);
    scope_wheel_y_ = modifier("scope/wheel_y", Qt::ShiftModifier);
    if (scope_wheel_x_ == scope_wheel_y_) {
        scope_wheel_x_ = Qt::ControlModifier;
        scope_wheel_y_ = Qt::ShiftModifier;
    }
    update_scope_shortcuts();
    for (auto &[id, action] : commands_) {
        auto key = QString::fromStdString(id);
        if (settings.contains(key))
            action->setShortcut(
                QKeySequence::fromString(settings.value(key).toString(), QKeySequence::PortableText));
    }
}
void EditorWindow::update_scope_shortcuts() {
    if (scope_)
        scope_->set_wheel_modifiers(scope_wheel_x_, scope_wheel_y_);
    for (auto &[id, view] : plot_views_)
        if (view)
            view->set_wheel_modifiers(scope_wheel_x_, scope_wheel_y_);
}
void EditorWindow::show_shortcuts() {
    QDialog dialog(this);
    dialog.setObjectName("shortcuts_dialog");
    dialog.setWindowTitle(text("shortcuts"));
    dialog.resize(670, 640);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(14);
    auto *title = new QLabel(text("shortcuts"));
    title->setStyleSheet("font-size:18px;font-weight:600;color:palette(text)");
    layout->addWidget(title);
    auto *table = new QTableWidget(static_cast<int>(commands_.size()), 2);
    table->setHorizontalHeaderLabels({text("action_label"), text("shortcut_label")});
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setShowGrid(false);
    layout->addWidget(table);
    std::map<std::string, QKeySequenceEdit *> editors;
    int row = 0;
    for (auto &[id, action] : commands_) {
        auto *name = new QTableWidgetItem(action->text());
        name->setFlags(Qt::ItemIsEnabled);
        table->setItem(row, 0, name);
        auto *edit = new QKeySequenceEdit(action->shortcut());
        edit->setObjectName("shortcut_" + QString::fromStdString(id));
        edit->setMaximumSequenceLength(1);
        table->setCellWidget(row, 1, edit);
        table->setRowHeight(row++, 35);
        editors[id] = edit;
    }
    auto *wheel_form = new QFormLayout;
    auto *wheel_x = new QComboBox;
    auto *wheel_y = new QComboBox;
    wheel_x->setObjectName("scope_wheel_x");
    wheel_y->setObjectName("scope_wheel_y");
    for (auto *combo : {wheel_x, wheel_y})
        for (int mask = 1; mask < 8; ++mask) {
            int flags = (mask & 1 ? Qt::ControlModifier : 0) | (mask & 2 ? Qt::ShiftModifier : 0) |
                        (mask & 4 ? Qt::AltModifier : 0);
            QStringList parts;
            if (mask & 1)
                parts << "Ctrl";
            if (mask & 2)
                parts << "Shift";
            if (mask & 4)
                parts << "Alt";
            combo->addItem(parts.join("+") + " + " + text("wheel"), flags);
        }
    wheel_x->setCurrentIndex(wheel_x->findData(int(scope_wheel_x_)));
    wheel_y->setCurrentIndex(wheel_y->findData(int(scope_wheel_y_)));
    wheel_form->addRow(text("wheel_zoom_x"), wheel_x);
    wheel_form->addRow(text("wheel_zoom_y"), wheel_y);
    layout->addLayout(wheel_form);
    auto *error = new QLabel;
    error->setObjectName("shortcut_error");
    error->setWordWrap(true);
    error->setStyleSheet("color:palette(bright-text)");
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel |
                                         QDialogButtonBox::RestoreDefaults);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &dialog, [&] {
        for (auto &[id, edit] : editors)
            edit->setKeySequence(default_shortcuts_.at(id));
        wheel_x->setCurrentIndex(wheel_x->findData(int(Qt::ControlModifier)));
        wheel_y->setCurrentIndex(wheel_y->findData(int(Qt::ShiftModifier)));
        error->clear();
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        for (auto a = editors.begin(); a != editors.end(); ++a)
            for (auto b = std::next(a); b != editors.end(); ++b) {
                auto x = a->second->keySequence(), y = b->second->keySequence();
                if (!x.isEmpty() && !y.isEmpty() &&
                    (x.matches(y) != QKeySequence::NoMatch || y.matches(x) != QKeySequence::NoMatch)) {
                    error->setText(text("shortcut_conflict") + " " + commands_.at(a->first)->text() + " / " +
                                   commands_.at(b->first)->text());
                    return;
                }
            }
        if (wheel_x->currentData() == wheel_y->currentData()) {
            error->setText(text("wheel_conflict"));
            return;
        }
        QDir().mkpath(recovery_dir_);
        QSettings settings(recovery_dir_ + "/shortcuts.ini", QSettings::IniFormat);
        for (auto &[id, edit] : editors) {
            commands_.at(id)->setShortcut(edit->keySequence());
            settings.setValue(QString::fromStdString(id),
                              edit->keySequence().toString(QKeySequence::PortableText));
        }
        scope_wheel_x_ = Qt::KeyboardModifiers(wheel_x->currentData().toInt());
        scope_wheel_y_ = Qt::KeyboardModifiers(wheel_y->currentData().toInt());
        settings.setValue("scope/wheel_x", int(scope_wheel_x_));
        settings.setValue("scope/wheel_y", int(scope_wheel_y_));
        update_scope_shortcuts();
        settings.sync();
        dialog.accept();
    });
    dialog.exec();
}
void EditorWindow::connect_gesture(WireAnchor from, WireAnchor to, std::vector<Point> bends,
                                   std::string replace) {
    if (running())
        return;
    try {
        for (auto &point : bends) {
            auto snapped = canvas_->snap_point({point.x, point.y});
            point = {snapped.x(), snapped.y()};
        }
        // Empty-space continuation is meaningful only for conserving electrical terminals.
        auto source = from.endpoint;
        if (source.object.empty())
            for (const auto &w : project().wires)
                if (w.id == from.wire) {
                    source = port_type(project(), w.from).direction == Direction::input ? w.to : w.from;
                    break;
                }
        if (to.endpoint.object.empty() && to.wire.empty() &&
            port_type(project(), source).domain != Domain::electrical)
            return;
        try {
            document_->connect_anchors(from, to, bends, replace);
        } catch (const Diagnostic &error) {
            if (error.code != "multiple_gate_drivers")
                throw;
            QMessageBox dialog(QMessageBox::Question, text("gate_replace_title"), text("gate_replace_text"),
                               QMessageBox::NoButton, this);
            dialog.setObjectName("gate_replace_dialog");
            auto *accept = dialog.addButton(text("gate_replace_action"), QMessageBox::AcceptRole);
            dialog.addButton(QMessageBox::Cancel);
            dialog.exec();
            if (dialog.clickedButton() != accept)
                return;
            document_->connect_anchors(from, to, bends, replace, true);
        }
        refresh();
    } catch (const std::exception &e) {
        show_error(e);
        refresh(false);
    }
}
void EditorWindow::quick_insert(QPointF point) {
    if (running())
        return;
    QDialog popup(this, Qt::Popup);
    popup.setObjectName("quick_insert");
    auto *layout = new QVBoxLayout(&popup);
    auto *search = new QLineEdit;
    search->setObjectName("quick_search");
    search->setPlaceholderText(text("search"));
    layout->addWidget(search);
    auto *list = new QListWidget;
    list->setObjectName("quick_results");
    layout->addWidget(list);
    for (const auto &[id, action] : component_actions_) {
        auto *item = new QListWidgetItem(action->text(), list);
        item->setData(Qt::UserRole, id);
    }
    connect(search, &QLineEdit::textChanged, &popup, [&](const QString &query) {
        QListWidgetItem *first = nullptr;
        for (int i = 0; i < list->count(); ++i) {
            auto *item = list->item(i);
            bool match = item->text().contains(query, Qt::CaseInsensitive);
            item->setHidden(!match);
            if (match && !first)
                first = item;
        }
        list->setCurrentItem(first);
    });
    int chosen = -1;
    auto accept = [&] {
        if (auto *item = list->currentItem()) {
            chosen = item->data(Qt::UserRole).toInt();
            popup.accept();
        }
    };
    connect(search, &QLineEdit::returnPressed, &popup, accept);
    connect(list, &QListWidget::itemActivated, &popup, [&](QListWidgetItem *) { accept(); });
    connect(list, &QListWidget::itemClicked, &popup, [&](QListWidgetItem *) { accept(); });
    list->setCurrentRow(0);
    popup.resize(300, 330);
    popup.move(canvas_->viewport()->mapToGlobal(canvas_->mapFromScene(point)));
    search->setFocus();
    if (popup.exec() == QDialog::Accepted && chosen >= 0) {
        begin_placement(chosen);
    }
}
} // namespace pds::desktop
