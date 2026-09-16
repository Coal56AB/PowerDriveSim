#include "apps/desktop/theme.hpp"
#include "apps/desktop/editor.hpp"
#include "apps/desktop/ui_icons.hpp"
#include "core/model/hierarchy.hpp"
#include <QCheckBox>
#include <QDialog>
#include <QGraphicsPathItem>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <iterator>
#include <set>
namespace pds::desktop {
void EditorWindow::append_simulation_result(Result batch) {
    batch.accepted_steps += continuation_steps_;
    if (!result_) {
        result_ = std::move(batch);
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
        auto connected = plot_channels(flat, plot.id);
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
    channels_->setMinimumWidth(240);
    channels_->setMaximumWidth(300);
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
        for (const auto &channel : available_channels(compile(root_project()))) {
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
                auto source = type.direction == Direction::output ? w.from : w.to;
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
        if (c.id == id)
            net({id, "p"});
    if (key.empty())
        return;
    set_scope_enabled(true);
    document_->apply("Observe point", [&](Project &p) {
        if (std::find(p.scope_channels.begin(), p.scope_channels.end(), key) == p.scope_channels.end())
            p.scope_channels.push_back(key);
    });
    refresh_channel_catalog();
    choose_channels();
    bottom_->setCurrentIndex(1);
    banner_->setText(text("record_next_run"));
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
