#include "apps/desktop/editor.hpp"
#include <QCheckBox>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <set>
namespace pds::desktop {
void EditorWindow::clear_result() {
    if (scope_)
        scope_->set_result(nullptr, {}, project());
    for (auto &[id, view] : plot_views_) {
        (void)id;
        if (view)
            view->set_result(nullptr, {}, project());
    }
    result_.reset();
    scope_export_->setEnabled(false);
}
std::vector<std::string> EditorWindow::recording_keys() const {
    std::set<std::string> keys;
    if (project().scope_enabled)
        keys.insert(project().scope_channels.begin(), project().scope_channels.end());
    for (const auto &plot : project().plots) {
        auto connected = plot_channels(project(), plot.id);
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
    if (!project().scope_enabled) {
        delete scope_content_;
        scope_content_ = nullptr;
        scope_ = nullptr;
        channels_ = nullptr;
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
    layout->addWidget(scope_, 1);
    scope_layout_->addWidget(scope_content_, 1);
    connect(channels_, &QListWidget::itemChanged, this, [this] {
        if (!rebuilding_)
            choose_channels();
    });
    scope_->changed = [this](double a, double b, double ca, double cb) {
        if (running())
            return;
        document_->apply("Scope view", [&](Project &p) {
            p.scope_begin = a;
            p.scope_end = b;
            p.cursor_a = ca;
            p.cursor_b = cb;
        });
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
    channels_->clear();
    try {
        for (const auto &channel : available_channels(compile(project()))) {
            auto *item = new QListWidgetItem(QString::fromStdString(channel.name + " [" + channel.unit + "]"),
                                             channels_);
            item->setData(Qt::UserRole, QString::fromStdString(channel.object));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            bool checked = std::find(project().scope_channels.begin(), project().scope_channels.end(),
                                     channel.object) != project().scope_channels.end();
            item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
            item->setToolTip(text("record_next_run"));
        }
    } catch (const Diagnostic &) { /* Incomplete circuits are diagnosed by Run. */
    }
    rebuilding_ = prior;
}
void EditorWindow::observe_object(const std::string &id) {
    if (running())
        return;
    auto resolved = resolve_connections(project());
    std::string key;
    auto net = [&](const Endpoint &endpoint) {
        auto i = resolved.nets.find(endpoint_key(endpoint));
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
                key = (port_type(project(), source).domain == Domain::gate ? "gate/" : "") + source.object;
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
void EditorWindow::open_plot(const std::string &id) {
    auto plot = std::find_if(project().plots.begin(), project().plots.end(),
                             [&](const PlotBlock &p) { return p.id == id; });
    if (plot == project().plots.end())
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
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(14);
    auto *header = new QHBoxLayout;
    auto *name = new QLabel(QString::fromStdString(plot->name));
    name->setObjectName("plot_heading");
    name->setStyleSheet("font-size:18px;font-weight:600;color:#253e60;");
    header->addWidget(name);
    header->addStretch();
    auto *export_button = new QPushButton(text("export_plot"));
    export_button->setObjectName("plot_export");
    header->addWidget(export_button);
    layout->addLayout(header);
    auto *legend = new QLabel;
    legend->setObjectName("plot_legend");
    legend->setWordWrap(true);
    layout->addWidget(legend);
    auto *view = new Scope;
    layout->addWidget(view, 1);
    auto *help = new QLabel(text("plot_navigation"));
    help->setStyleSheet("color:#7b8da4;");
    layout->addWidget(help);
    plot_windows_[id] = window;
    plot_views_[id] = view;
    connect(export_button, &QPushButton::clicked, this,
            [this, id] { export_csv(plot_channels(project(), id)); });
    view->changed = [this, id](double a, double b, double ca, double cb) {
        if (running())
            return;
        document_->apply("Plot view", [&](Project &p) {
            for (auto &plot : p.plots)
                if (plot.id == id) {
                    plot.begin = a;
                    plot.end = b;
                    plot.cursor_a = ca;
                    plot.cursor_b = cb;
                }
        });
        update_title();
    };
    update_graphs();
    window->show();
}
void EditorWindow::update_graphs() {
    const QStringList colors{"#146cca", "#c56819", "#17866d", "#935ad5", "#d04769"};
    for (auto &[id, window] : plot_windows_) {
        if (!window)
            continue;
        auto plot = std::find_if(project().plots.begin(), project().plots.end(),
                                 [&](const PlotBlock &p) { return p.id == id; });
        if (plot == project().plots.end()) {
            window->close();
            continue;
        }
        window->setWindowTitle(QString::fromStdString(plot->name) + " · PowerDriveSim");
        window->findChild<QLabel *>("plot_heading")->setText(QString::fromStdString(plot->name));
        auto keys = plot_channels(project(), id);
        auto indexes = result_indices(keys);
        QStringList labels;
        for (size_t i = 0; i < indexes.size(); ++i) {
            int index = indexes[i];
            QString name = index < static_cast<int>(result_->channels.size())
                               ? QString::fromStdString(result_->channels[index].name + " [" +
                                                        result_->channels[index].unit + "]")
                               : QString::fromStdString("gate [bool]");
            labels << "<span style='color:" + colors[static_cast<int>(i) % colors.size()] + "'>● " +
                          name.toHtmlEscaped() + "</span>";
        }
        window->findChild<QLabel *>("plot_legend")
            ->setText(labels.empty() ? text("plot_connect_hint") : labels.join(" &nbsp; &nbsp; "));
        window->findChild<QPushButton *>("plot_export")
            ->setEnabled(result_ && !result_->samples.empty() && !indexes.empty());
        Project view_state;
        view_state.scope_begin = plot->begin;
        view_state.scope_end = plot->end;
        view_state.cursor_a = plot->cursor_a;
        view_state.cursor_b = plot->cursor_b;
        if (plot_views_[id])
            plot_views_[id]->set_result(result_ ? &*result_ : nullptr, indexes, view_state);
    }
}
} // namespace pds::desktop
