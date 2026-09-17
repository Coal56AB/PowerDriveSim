#include "apps/desktop/theme.hpp"
#include "apps/desktop/editor.hpp"
#include "apps/desktop/instrumentation.hpp"
#include "apps/desktop/ui_icons.hpp"
#include "core/model/hierarchy.hpp"
#include <QCheckBox>
#include <QDialog>
#include <QGraphicsPathItem>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>
namespace pds::desktop {
namespace {
void append_differential_channels(Result &result, const Project &root) {
    const auto flat = flatten(root).project;
    const size_t analog_count = result.channels.size();
    auto source = [&](const std::string &key) -> std::optional<std::pair<bool, size_t>> {
        for (size_t i = 0; i < analog_count; ++i)
            if (result.channels[i].object == key)
                return std::pair{false, i};
        for (size_t i = 0; i < result.gate_objects.size(); ++i)
            if ("gate/" + result.gate_objects[i] == key)
                return std::pair{true, i};
        return {};
    };
    for (const auto &plot : flat.plots) {
        if (!plot.differential)
            continue;
        const auto pairs = plot_differential_channels(flat, plot.id);
        for (size_t i = 0; i < pairs.size(); ++i) {
            if (pairs[i].first.empty() || pairs[i].second.empty())
                continue;
            const auto positive = source(pairs[i].first), negative = source(pairs[i].second);
            if (!positive || !negative)
                continue;
            auto unit = [&](const std::pair<bool, size_t> &index) -> std::string {
                return index.first ? std::string() : result.channels[index.second].unit;
            };
            const auto positive_unit = unit(*positive), negative_unit = unit(*negative);
            result.channels.push_back({"diff/" + plot.id + "/" + std::to_string(i + 1),
                                       plot.name + " Δ" + std::to_string(i + 1),
                                       positive_unit == negative_unit ? positive_unit : std::string()});
            for (auto &sample : result.samples) {
                auto value = [&](const std::pair<bool, size_t> &index) {
                    return index.first ? (sample.gates[index.second] ? 1.0 : 0.0)
                                       : sample.values[index.second];
                };
                sample.values.push_back(value(*positive) - value(*negative));
            }
        }
    }
}
}
void EditorWindow::append_simulation_result(Result batch) {
    append_differential_channels(batch, root_project());
    accumulate_statistics(batch, continuation_statistics_);
    if (!result_) {
        result_ = std::move(batch);
        // Reserve sample headers once for ordinary fixed-step runs. Per-sample
        // channel payloads are still allocated by the worker as results arrive.
        const auto &profile = root_project().profile;
        if (!result_->samples.empty() && !profile.step_control.adaptive && profile.step > 0) {
            const double estimate = std::ceil(profile.stop / profile.step) * 1.02 + 1024;
            constexpr size_t maximum_reservation = 20'000'000;
            if (std::isfinite(estimate) && estimate > double(result_->samples.capacity()) &&
                estimate <= double(maximum_reservation)) {
                try { result_->samples.reserve(size_t(estimate)); }
                catch (const std::bad_alloc &) { /* Retain ordinary incremental growth. */ }
            }
        }
        return;
    }
    auto samples = std::move(result_->samples);
    auto first = batch.samples.begin();
    if (!samples.empty() && first != batch.samples.end() && samples.back().time == first->time)
        ++first; // A resumed run publishes its checkpoint endpoint once more.
    samples.insert(samples.end(), std::make_move_iterator(first),
                   std::make_move_iterator(batch.samples.end()));
    result_ = std::move(batch);
    result_->samples = std::move(samples);
}
void EditorWindow::drain_simulation_stream() {
    std::deque<Result> batches;
    {
        std::lock_guard lock(stream_mutex_);
        batches.swap(stream_queue_);
    }
    if (batches.empty())
        return;
    for (auto &batch : batches)
        append_simulation_result(std::move(batch));
    if (scope_)
        scope_->set_result(&*result_, result_indices(project().scope_channels), project());
    update_graphs();
}
void EditorWindow::clear_result() {
    if (scope_)
        scope_->set_result(nullptr, {}, project());
    for (auto &[id, view] : plot_views_) {
        (void)id;
        if (view)
            view->set_result(nullptr, {}, project());
    }
    if (result_ && result_->samples.size() > 100000) {
        // Detach views first, then release a large old history away from the UI
        // thread. Starting the next worker must not wait for millions of frees.
        (void)QtConcurrent::run([retired = std::move(*result_)]() mutable { retired.samples.clear(); });
    }
    result_.reset();
    result_project_.reset();
    scope_export_->setEnabled(false);
    scope_fit_->setEnabled(false);
}
std::vector<std::string> EditorWindow::recording_keys() const {
    std::set<std::string> keys;
    if (project().scope_enabled)
        keys.insert(project().scope_channels.begin(), project().scope_channels.end());
    const auto flat = flatten(root_project()).project;
    for (const auto &plot : flat.plots) {
        auto connected = plot_source_channels(flat, plot.id);
        keys.insert(connected.begin(), connected.end());
    }
    return {keys.begin(), keys.end()};
}
std::vector<int> EditorWindow::result_indices(const std::vector<std::string> &keys) const {
    std::vector<int> result;
    if (!result_)
        return result;
    for (const auto &key : keys) {
        for (size_t i = 0; i < result_->channels.size(); ++i)
            if (result_->channels[i].object == key)
                result.push_back(static_cast<int>(i));
        for (size_t i = 0; i < result_->gate_objects.size(); ++i)
            if ("gate/" + result_->gate_objects[i] == key)
                result.push_back(static_cast<int>(result_->channels.size() + i));
    }
    return result;
}
void EditorWindow::sync_scope() {
    bool prior = rebuilding_;
    rebuilding_ = true;
    scope_enable_->setChecked(project().scope_enabled);
    rebuilding_ = prior;
    scope_hint_->setVisible(!project().scope_enabled);
    scope_fit_->hide();
    if (!project().scope_enabled) {
        delete scope_content_;
        scope_content_ = nullptr;
        scope_ = nullptr;
        channels_ = nullptr;
        update_wires();
        scope_export_->setEnabled(false);
        return;
    }
    if (scope_content_)
        return;
    scope_content_ = new QWidget;
    auto *layout = new QHBoxLayout(scope_content_);
    layout->setContentsMargins(0, 0, 0, 0);
    channels_ = new QListWidget;
    channels_->setObjectName("channels");
    channels_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    channels_->setMinimumWidth(240);
    channels_->setMaximumWidth(300);
    channels_->setContextMenuPolicy(Qt::CustomContextMenu);
    channels_->installEventFilter(this);
    channels_->viewport()->installEventFilter(this);
    layout->addWidget(channels_);
    scope_ = new Scope;
    scope_->set_wheel_modifiers(scope_wheel_x_, scope_wheel_y_);
    for (const auto &options : project().view_options)
        if (options.plot.empty())
            scope_->load_view_options(options);
    auto *chart = new QVBoxLayout;
    chart->addWidget(scope_->navigation());
    chart->addWidget(scope_, 1);
    layout->addLayout(chart, 1);
    scope_layout_->addWidget(scope_content_, 1);
    connect(channels_, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        if (running()) {
            QSignalBlocker block(channels_);
            const auto key = item->data(Qt::UserRole).toString().toStdString();
            const bool checked = std::find(project().scope_channels.begin(), project().scope_channels.end(),
                                           key) != project().scope_channels.end();
            item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        } else if (!rebuilding_)
            choose_channels();
    });
    connect(channels_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (rebuilding_)
            return;
        update_wires();
        if (!item)
            return;
        // Keep keyboard focus and edit selection unchanged: this is a source preview.
        for (const auto &[id, atom] : atoms_)
            if (atom->data(channel_highlight_role).toBool()) {
                canvas_->ensureVisible(atom);
                return;
            }
        for (const auto &[id, wire] : wires_)
            if (wire->data(channel_highlight_role).toBool()) {
                canvas_->ensureVisible(wire);
                return;
            }
    });
    connect(channels_, &QListWidget::customContextMenuRequested, this, [this](QPoint point) {
        if (auto *item = channels_->itemAt(point); item && !item->isSelected()) {
            channels_->clearSelection();
            channels_->setCurrentItem(item);
            item->setSelected(true);
        }
        if (!channels_->currentItem())
            return;
        QMenu menu(channels_);
        std::vector<std::string> keys;
        for (auto *item : channels_->selectedItems())
            keys.push_back(item->data(Qt::UserRole).toString().toStdString());
        if (keys.empty())
            keys.push_back(channels_->currentItem()->data(Qt::UserRole).toString().toStdString());
        const auto key = keys.front();
        auto *multiplier = menu.addAction(text("curve_multiplier_action"));
        connect(multiplier, &QAction::triggered, this,
                [this, keys] { scope_->show_multiplier_settings(keys); });
        auto *appearance = menu.addAction(text("curve_appearance_action"));
        connect(appearance, &QAction::triggered, this, [this, key] { scope_->show_curve_settings(key); });
        menu.addSeparator();
        auto *remove = menu.addAction(text("delete"));
        connect(remove, &QAction::triggered, this, &EditorWindow::remove_scope_point);
        menu.exec(channels_->viewport()->mapToGlobal(point));
    });
    scope_->changed = [this](double a, double b, double ca, double cb) {
        document_->set_view("", a, b, ca, cb);
        document_->set_view_options(scope_->view_options());
        update_title();
    };
}
void EditorWindow::set_scope_enabled(bool enabled) {
    if (running())
        return;
    if (project().scope_enabled != enabled)
        document_->apply("Scope recording", [&](Project &p) { p.scope_enabled = enabled; });
    sync_scope();
    refresh_channel_catalog();
    if (!enabled && result_) {
        for (auto &[id, view] : plot_views_)
            if (view)
                view->set_result(nullptr, {}, project());
        result_ = select_result(*result_, recording_keys());
        update_graphs();
    }
    if (enabled)
        choose_channels();
    update_title();
}
void EditorWindow::refresh_channel_catalog() {
    if (!channels_)
        return;
    bool prior = rebuilding_;
    rebuilding_ = true;
    const auto selected_channel =
        channels_->currentItem() ? channels_->currentItem()->data(Qt::UserRole) : QVariant();
    channels_->clear();
    try {
        const auto available = available_channels(compile(root_project()));
        std::vector<std::string> points = project().scope_points;
        for (const auto &active : project().scope_channels)
            if (std::find(points.begin(), points.end(), active) == points.end())
                points.push_back(active);
        for (const auto &key : points) {
            auto found = std::find_if(available.begin(), available.end(),
                                      [&](const Channel &channel) { return channel.object == key; });
            if (found == available.end())
                continue;
            const auto &channel = *found;
            auto *item = new QListWidgetItem(QString::fromStdString(channel.name + " [" + channel.unit + "]"),
                                             channels_);
            item->setData(Qt::UserRole, QString::fromStdString(channel.object));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            bool checked = std::find(project().scope_channels.begin(), project().scope_channels.end(),
                                     channel.object) != project().scope_channels.end();
            item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
            item->setToolTip(text("channel_source_hint") + "\n" + text("record_next_run"));
            if (item->data(Qt::UserRole) == selected_channel)
                channels_->setCurrentItem(item);
        }
    } catch (const Diagnostic &) { /* Incomplete circuits are diagnosed by Run. */
    }
    rebuilding_ = prior;
    update_wires();
}
void EditorWindow::observe_object(const std::string &id) {
    if (running())
        return;
    auto resolved = resolve_connections(root_project());
    std::string key;
    auto net = [&](const Endpoint &endpoint) {
        auto global = endpoint;
        global.object = expanded_uuid(hierarchy_path(), endpoint.object);
        auto i = resolved.nets.find(endpoint_key(global));
        if (i != resolved.nets.end())
            key = i->second;
    };
    for (const auto &w : project().wires)
        if (w.id == id) {
            auto type = port_type(project(), w.from);
            if (type.domain == Domain::electrical)
                net(w.from);
            else {
                auto source = type.domain == Domain::gate
                                  ? (type.direction == Direction::input ? w.from : w.to)
                                  : (type.direction == Direction::output ? w.from : w.to);
                auto global = source;
                global.object = expanded_uuid(hierarchy_path(), source.object);
                auto expanded = flatten(root_project());
                auto alias = expanded.terminals.find(endpoint_key(global));
                key = (port_type(project(), source).domain == Domain::gate ? "gate/" : "") +
                      (alias == expanded.terminals.end() ? global.object : alias->second.object);
            }
        }
    for (const auto &node : project().nodes)
        if (node.id == id)
            net({id, "node"});
    for (const auto &c : project().components)
        if (c.id == id) {
            key = expanded_uuid(hierarchy_path(), c.id);
        }
    if (key.empty())
        return;
    set_scope_enabled(true);
    document_->apply("Observe point", [&](Project &p) {
        if (std::find(p.scope_points.begin(), p.scope_points.end(), key) == p.scope_points.end())
            p.scope_points.push_back(key);
        if (std::find(p.scope_channels.begin(), p.scope_channels.end(), key) == p.scope_channels.end())
            p.scope_channels.push_back(key);
    });
    refresh_channel_catalog();
    choose_channels();
    bottom_->setCurrentIndex(1);
    banner_->setText(text("record_next_run"));
}
void EditorWindow::observe_component_terminals(const std::string &id) {
    if (running())
        return;
    const auto resolved = resolve_connections(root_project());
    std::vector<std::string> keys;
    auto add = [&](const Endpoint &local) {
        Endpoint global{expanded_uuid(hierarchy_path(), local.object), local.port};
        auto found = resolved.nets.find(endpoint_key(global));
        if (found != resolved.nets.end() && std::find(keys.begin(), keys.end(), found->second) == keys.end())
            keys.push_back(found->second);
    };
    for (const auto &component : project().components)
        if (component.id == id) {
            add({id, "p"});
            add({id, "n"});
        }
    for (const auto &instance : project().instances)
        if (instance.id == id)
            for (const auto &port : definition(project(), instance.definition).ports)
                if (port.domain == Domain::electrical)
                    add({id, port.id});
    if (keys.empty())
        return;
    set_scope_enabled(true);
    document_->apply("Observe terminals", [&](Project &p) {
        for (const auto &key : keys) {
            if (std::find(p.scope_points.begin(), p.scope_points.end(), key) == p.scope_points.end())
                p.scope_points.push_back(key);
            if (std::find(p.scope_channels.begin(), p.scope_channels.end(), key) == p.scope_channels.end())
                p.scope_channels.push_back(key);
        }
    });
    refresh_channel_catalog();
    choose_channels();
    bottom_->setCurrentIndex(1);
    banner_->setText(text("record_next_run"));
}
void EditorWindow::observe_wire_current(const std::string &id) {
    observe_wires({id}, true);
}
void EditorWindow::observe_wires(const std::vector<std::string> &ids, bool current) {
    if (running() || ids.empty())
        return;
    if (!current) {
        const auto resolved = resolve_connections(root_project());
        std::vector<std::string> keys;
        for (const auto &id : ids) {
            auto found = std::find_if(project().wires.begin(), project().wires.end(),
                                      [&](const Wire &wire) { return wire.id == id; });
            if (found == project().wires.end())
                continue;
            auto type = port_type(project(), found->from);
            std::string key;
            if (type.domain == Domain::electrical) {
                Endpoint global{expanded_uuid(hierarchy_path(), found->from.object), found->from.port};
                if (auto net = resolved.nets.find(endpoint_key(global)); net != resolved.nets.end())
                    key = net->second;
            } else {
                auto source = type.domain == Domain::gate
                                  ? (type.direction == Direction::input ? found->from : found->to)
                                  : (type.direction == Direction::output ? found->from : found->to);
                auto global = source;
                global.object = expanded_uuid(hierarchy_path(), source.object);
                const auto expanded = flatten(root_project());
                const auto alias = expanded.terminals.find(endpoint_key(global));
                key = (port_type(project(), source).domain == Domain::gate ? "gate/" : "") +
                      (alias == expanded.terminals.end() ? global.object : alias->second.object);
            }
            if (!key.empty() && std::find(keys.begin(), keys.end(), key) == keys.end())
                keys.push_back(std::move(key));
        }
        if (keys.empty())
            return;
        document_->apply("Observe wires", [&](Project &p) {
            p.scope_enabled = true;
            for (const auto &key : keys) {
                if (std::find(p.scope_points.begin(), p.scope_points.end(), key) == p.scope_points.end())
                    p.scope_points.push_back(key);
                if (std::find(p.scope_channels.begin(), p.scope_channels.end(), key) == p.scope_channels.end())
                    p.scope_channels.push_back(key);
            }
        });
        refresh();
        bottom_->setCurrentIndex(1);
        banner_->setText(text("record_next_run"));
        return;
    }
    struct Request {
        std::string wire, probe;
        Wire original;
        QPointF center;
        unsigned turn = 0;
    };
    std::vector<Request> requests;
    for (const auto &id : ids) {
        if (!wires_.count(id) || std::any_of(requests.begin(), requests.end(),
                                             [&](const Request &request) { return request.wire == id; }))
            continue;
        auto found = std::find_if(project().wires.begin(), project().wires.end(),
                                  [&](const Wire &wire) { return wire.id == id; });
        if (found == project().wires.end() || port_type(project(), found->from).domain != Domain::electrical ||
            port_type(project(), found->to).domain != Domain::electrical)
            continue;
        const auto path = wires_.at(id)->path();
        if (path.elementCount() < 2)
            continue;
        QPointF center, direction;
        double longest = -1;
        for (int i = 1; i < path.elementCount(); ++i) {
            const auto ea = path.elementAt(i - 1), eb = path.elementAt(i);
            const QPointF a(ea.x, ea.y), b(eb.x, eb.y);
            const double length = QLineF(a, b).length();
            if (length > longest) {
                longest = length;
                center = (a + b) / 2.0;
                direction = b - a;
            }
        }
        unsigned turn = 0;
        if (std::abs(direction.x()) >= std::abs(direction.y()))
            turn = direction.x() >= 0 ? 0u : 2u;
        else
            turn = direction.y() >= 0 ? 1u : 3u;
        requests.push_back({id, new_uuid(), *found, canvas_->snap_point(center), turn});
    }
    if (requests.empty())
        return;
    try {
        document_->apply("Observe wire currents", [&](Project &p) {
            p.scope_enabled = true;
            for (const auto &request : requests) {
                std::erase_if(p.wires, [&](const Wire &wire) { return wire.id == request.wire; });
                Component sensor{request.probe, "IP" + std::to_string(p.components.size() + 1),
                                 Kind::current_probe, "", "", 0, 0,
                                 request.center.x(), request.center.y(), false};
                sensor.orientation.quarter_turns = request.turn;
                p.components.push_back(std::move(sensor));
                mark_hidden_current_probe(p, request.probe);
                auto add_wire = [&](std::string id, Endpoint from, Endpoint to, std::vector<Point> bends) {
                    Wire wire{std::move(id), std::move(from), std::move(to), std::move(bends)};
                    wire.color = request.original.color;
                    wire.width = request.original.width;
                    wire.line = request.original.line;
                    p.wires.push_back(std::move(wire));
                };
                // The primary half retains the original identity and complete
                // visual route. The editor renders both solver branches as
                // this single seamless conductor.
                add_wire(request.original.id, request.original.from, {request.probe, "p"},
                         request.original.bends);
                add_wire(new_uuid(), {request.probe, "n"}, request.original.to, {});
                p.scope_points.push_back(request.probe);
                p.scope_channels.push_back(request.probe);
            }
        });
        selected_.clear();
        refresh();
        bottom_->setCurrentIndex(1);
        banner_->setText(text("record_next_run"));
    } catch (const std::exception &error) {
        show_error(error);
    }
}
void EditorWindow::remove_scope_point() {
    if (!channels_ || !channels_->currentItem() || running())
        return;
    std::vector<std::string> keys;
    for (auto *item : channels_->selectedItems())
        keys.push_back(item->data(Qt::UserRole).toString().toStdString());
    if (keys.empty())
        keys.push_back(channels_->currentItem()->data(Qt::UserRole).toString().toStdString());
    document_->apply("Remove scope point", [&](Project &p) {
        for (const auto &key : keys) {
            std::erase(p.scope_points, key);
            std::erase(p.scope_channels, key);
            auto sensor = std::find_if(p.components.begin(), p.components.end(), [&](const Component &component) {
                return component.id == key && component.kind == Kind::current_probe &&
                       is_hidden_current_probe(p, component.id);
            });
            if (sensor == p.components.end())
                continue;
            const auto view = [&]() -> std::optional<HiddenCurrentWireView> {
                for (const auto &wire : p.wires)
                    if (wire.from.object == key || wire.to.object == key)
                        if (auto candidate = hidden_current_wire_view(p, wire.id))
                            return candidate;
                return {};
            }();
            if (view) {
                const auto primary = std::find_if(p.wires.begin(), p.wires.end(),
                                                  [&](const Wire &wire) { return wire.id == view->primary; });
                Wire merged{view->primary, view->from, view->to, view->bends};
                if (primary != p.wires.end()) {
                    merged.color = primary->color;
                    merged.width = primary->width;
                    merged.line = primary->line;
                }
                std::erase_if(p.wires, [&](const Wire &wire) {
                    return wire.from.object == key || wire.to.object == key;
                });
                if (merged.from != merged.to)
                    p.wires.push_back(std::move(merged));
            }
            std::erase_if(p.components, [&](const Component &component) { return component.id == key; });
            std::erase_if(p.labels, [&](const LabelLayout &label) { return label.object == key; });
            unmark_hidden_current_probe(p, key);
        }
    });
    refresh();
}
void EditorWindow::open_plot(const std::string &local_id) {
    const auto id = expanded_uuid(hierarchy_path(), local_id);
    const auto flat = flatten(root_project()).project;
    auto plot =
        std::find_if(flat.plots.begin(), flat.plots.end(), [&](const PlotBlock &p) { return p.id == id; });
    if (plot == flat.plots.end())
        return;
    if (plot_windows_[id]) {
        plot_windows_[id]->show();
        plot_windows_[id]->raise();
        return;
    }
    auto *window = new QDialog(this, Qt::Window);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setObjectName("plot_" + QString::fromStdString(id));
    window->resize(940, 520);
    window->setMinimumSize(650, 370);
    auto *layout = new QVBoxLayout(window);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(4);
    auto *header = new QHBoxLayout;
    auto *name = new QLabel(QString::fromStdString(plot->name));
    name->setObjectName("plot_heading");
    name->setStyleSheet("font-size:18px;font-weight:600;color:palette(text);");
    header->addWidget(name);
    header->addStretch();
    auto *export_button = new QPushButton(ui_icon(UiIcon::export_data), QString());
    export_button->setToolTip(text("export_plot"));
    export_button->setAccessibleName(text("export_plot"));
    export_button->setIconSize({22, 22});
    export_button->setFixedSize(32, 32);
    export_button->setStyleSheet("padding:3px;");
    export_button->setObjectName("plot_export");
    export_button->setAutoDefault(false);
    export_button->setDefault(false);
    header->addWidget(export_button);
    layout->addLayout(header);
    auto *view = new Scope;
    layout->addWidget(view->channel_controls());
    view->set_wheel_modifiers(scope_wheel_x_, scope_wheel_y_);
    view->set_live(running());
    for (const auto &options : flat.view_options)
        if (options.plot == id)
            view->load_view_options(options);
    layout->addWidget(view->navigation());
    layout->addWidget(view, 1);
    view->setToolTip(text("plot_navigation"));
    plot_windows_[id] = window;
    plot_views_[id] = view;
    connect(export_button, &QPushButton::clicked, this,
            [this, id] { export_csv(plot_channels(root_project(), id)); });
    view->changed = [this, id](double a, double b, double ca, double cb) {
        document_->set_view(id, a, b, ca, cb);
        if (plot_views_[id]) {
            auto options = plot_views_[id]->view_options();
            options.plot = id;
            document_->set_view_options(options);
        }
        update_title();
    };
    update_graphs();
    window->show();
}
void EditorWindow::update_graphs() {
    if (plot_windows_.empty())
        return;
    const auto flat = flatten(root_project()).project;
    for (auto &[id, window] : plot_windows_) {
        if (!window)
            continue;
        auto plot = std::find_if(flat.plots.begin(), flat.plots.end(),
                                 [&](const PlotBlock &p) { return p.id == id; });
        if (plot == flat.plots.end()) {
            window->close();
            continue;
        }
        window->setWindowTitle(QString::fromStdString(plot->name) + " · PowerDriveSim");
        window->findChild<QLabel *>("plot_heading")->setText(QString::fromStdString(plot->name));
        auto keys = plot_channels(flat, id);
        auto indexes = result_indices(keys);
        window->findChild<QPushButton *>("plot_export")
            ->setEnabled(result_ && !result_->samples.empty() && !indexes.empty());
        Project view_state;
        view_state.scope_begin = plot->begin;
        view_state.scope_end = plot->end;
        view_state.cursor_a = plot->cursor_a;
        view_state.cursor_b = plot->cursor_b;
        for (const auto &options : flat.view_options)
            if (options.plot == id && options.viewport) {
                view_state.scope_begin = options.begin;
                view_state.scope_end = options.end;
                view_state.cursor_a = options.cursor_a;
                view_state.cursor_b = options.cursor_b;
            }
        if (plot_views_[id])
            plot_views_[id]->set_result(result_ ? &*result_ : nullptr, indexes, view_state);
    }
}
} // namespace pds::desktop
