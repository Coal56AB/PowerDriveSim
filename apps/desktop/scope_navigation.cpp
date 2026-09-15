#include "apps/desktop/curve_name_button.hpp"
#include "apps/desktop/editor.hpp"
#include "apps/desktop/number_input.hpp"
#include "apps/desktop/ui_icons.hpp"
#include "results/measurements.hpp"
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidgetAction>
#include <algorithm>
#include <cmath>
namespace pds::desktop {
QRectF Scope::plot_rect() const {
    return {58, 12, double(std::max(1, width() - 84)), double(std::max(1, height() - 64))};
}
QWidget *Scope::channel_controls() {
    channel_bar_ = new QToolBar;
    channel_bar_->setObjectName("plot_legend");
    channel_bar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    channel_bar_->setIconSize({18, 18});
    channel_bar_->setStyleSheet(
        "QToolBar{background:transparent;border:0;spacing:6px;}"
        "QFrame#curve_channel{border:1px solid #d3dfef;border-radius:4px;background:transparent;}"
        "QFrame#curve_channel:hover{background:#eaf3ff;border-color:#9bbde8;}"
        "QFrame#curve_channel QToolButton{padding:3px 5px;border:0;border-radius:3px;background:transparent;}"
        "QFrame#curve_channel QToolButton:hover{background:#dceafe;}"
        "QFrame#curve_channel QToolButton:pressed{background:#cbdffc;}");
    update_channel_controls();
    return channel_bar_;
}
bool Scope::channel_visible(const std::string &key) const {
    return !hidden_channels_.contains(key);
}
void Scope::set_channel_visible(const std::string &key, bool visible) {
    if (key.empty() || channel_visible(key) == visible)
        return;
    if (visible)
        hidden_channels_.erase(key);
    else
        hidden_channels_.insert(key);
    if (pending_options_)
        pending_options_->hidden_channels.assign(hidden_channels_.begin(), hidden_channels_.end());
    update_channel_controls();
    if (changed)
        changed(begin, end, cursor_a, cursor_b);
    update();
}
void Scope::update_channel_controls() {
    if (!channel_bar_)
        return;
    QStringList keys;
    if (result_)
        for (int channel : channels_)
            keys << QString::fromStdString(result_channel(*result_, channel).object);
    QStringList old_keys;
    for (auto *action : channel_bar_->actions())
        if (action->isCheckable())
            old_keys << action->data().toString();
    if (keys != old_keys) {
        for (auto *action : channel_bar_->actions())
            delete action;
        for (int channel : channels_) {
            if (!result_)
                break;
            const auto info = result_channel(*result_, channel);
            auto *group = new QFrame;
            group->setObjectName("curve_channel");
            auto *layout = new QHBoxLayout(group);
            layout->setContentsMargins(1, 1, 1, 1);
            layout->setSpacing(0);
            auto *action = new QWidgetAction(channel_bar_);
            action->setText(QString::fromStdString(info.name + " [" + info.unit + "]"));
            action->setData(QString::fromStdString(info.object));
            action->setCheckable(true);
            auto *visibility = new CurveNameButton(group);
            visibility->setObjectName("curve_visibility");
            visibility->setDefaultAction(action);
            visibility->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            visibility->setIconSize({18, 18});
            layout->addWidget(visibility);
            auto *name_edit = new CurveNameEdit(group);
            name_edit->setObjectName("curve_name_edit");
            name_edit->setMaxLength(256);
            name_edit->setToolTip(text("curve_rename_hint"));
            name_edit->hide();
            layout->addWidget(name_edit);
            visibility->rename = [this, visibility, name_edit, key = info.object] {
                name_edit->setText(curve_name(key));
                name_edit->setMinimumWidth(visibility->width());
                visibility->hide();
                name_edit->show();
                name_edit->setFocus();
                name_edit->selectAll();
            };
            name_edit->cancel = [visibility, name_edit] {
                name_edit->hide();
                visibility->show();
                visibility->setFocus();
            };
            connect(name_edit, &QLineEdit::editingFinished, this,
                    [this, visibility, name_edit, key = info.object] {
                        if (!name_edit->isVisible())
                            return;
                        const auto name = name_edit->text();
                        name_edit->hide();
                        visibility->show();
                        set_curve_name(key, name);
                    });
            connect(action, &QAction::triggered, this,
                    [this, key = info.object](bool visible) { set_channel_visible(key, visible); });
            auto *settings =
                new QAction(ui_icon(UiIcon::gear), text("curve_style").arg(action->text()), group);
            settings->setObjectName("curve_style_" + QString::fromStdString(info.object));
            settings->setData(QString::fromStdString(info.object));
            auto *button = new QToolButton(group);
            button->setDefaultAction(settings);
            button->setToolButtonStyle(Qt::ToolButtonIconOnly);
            button->setIconSize({18, 18});
            layout->addWidget(button);
            action->setDefaultWidget(group);
            channel_bar_->addAction(action);
            connect(settings, &QAction::triggered, this,
                    [this, key = info.object] { show_curve_settings(key); });
        }
    }
    const QStringList colors{"#146cca", "#c56819", "#17866d", "#935ad5", "#d04769"};
    int index = 0;
    for (auto *action : channel_bar_->actions()) {
        if (!action->isCheckable())
            continue;
        const bool visible = channel_visible(action->data().toString().toStdString());
        const auto info = result_channel(*result_, channels_[index]);
        action->setText(curve_name(info.object) + QString::fromStdString(" [" + info.unit + "]"));
        auto *group = channel_bar_->widgetForAction(action);
        if (auto *settings =
                group->findChild<QAction *>("curve_style_" + QString::fromStdString(info.object)))
            settings->setText(text("curve_style").arg(action->text()));
        action->setChecked(visible);
        action->setIcon(ui_icon(visible ? UiIcon::visible : UiIcon::hidden));
        action->setToolTip(text(visible ? "hide_signal" : "show_signal").arg(action->text()) + "\n" + text("curve_rename_hint"));
        auto font = action->font();
        font.setStrikeOut(!visible);
        action->setFont(font);
        if (auto *button =
                channel_bar_->widgetForAction(action)->findChild<QToolButton *>("curve_visibility"))
            button->setStyleSheet("color:" + (visible ? colors[index % colors.size()] : "#8793a3") + ";");
        ++index;
    }
    channel_bar_->setVisible(!keys.empty());
}
QWidget *Scope::navigation() {
    auto *container = new QWidget;
    container->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto *bar = new QToolBar;
    bar->setObjectName("plot_navigation");
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    bar->setIconSize({22, 22});
    bar->setStyleSheet(
        "QToolBar{background:transparent;border:0;spacing:2px;}QToolButton{padding:3px;"
        "min-height:22px;min-width:22px;}QToolButton:checked{background:#dceafe;border:1px solid "
        "#6f9ad0;color:#21496f;}");
    layout->addWidget(bar);
    auto *group = new QActionGroup(bar);
    group->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
    for (auto axes : {Axes::x, Axes::y, Axes::xy}) {
        const QString suffix = axes == Axes::x ? "X" : axes == Axes::y ? "Y" : "XY";
        auto *a = bar->addAction(ui_icon(axes == Axes::x   ? UiIcon::zoom_x
                                         : axes == Axes::y ? UiIcon::zoom_y
                                                           : UiIcon::zoom_xy),
                                 "Zoom " + suffix);
        a->setObjectName("zoom_" + suffix.toLower());
        a->setCheckable(true);
        group->addAction(a);
        a->setChecked(zoom_axes == axes && navigation_tool_ == NavigationTool::zoom);
        zoom_actions_[static_cast<int>(axes)] = a;
        a->setToolTip(text("zoom_axis").arg(suffix));
        connect(a, &QAction::triggered, this, [this, axes](bool enabled) {
            zoom_axes = axes;
            set_navigation_tool(enabled ? NavigationTool::zoom : NavigationTool::pan);
        });
    }
    cursor_action_ = bar->addAction(ui_icon(UiIcon::cursors), text("cursors"));
    cursor_action_->setObjectName("cursor_mode");
    cursor_action_->setCheckable(true);
    group->addAction(cursor_action_);
    connect(cursor_action_, &QAction::triggered, this, &Scope::set_cursor_mode);
    bar->addSeparator();
    for (auto axes : {Axes::x, Axes::y, Axes::xy}) {
        const QString suffix = axes == Axes::x ? "X" : axes == Axes::y ? "Y" : "XY";
        auto *a = bar->addAction(ui_icon(axes == Axes::x   ? UiIcon::fit_x
                                         : axes == Axes::y ? UiIcon::fit_y
                                                           : UiIcon::fit_xy),
                                 text("fit_axis").arg(suffix));
        a->setObjectName("fit_" + suffix.toLower());
        connect(a, &QAction::triggered, this, [this, axes] {
            cancel_drag();
            fit(axes);
        });
    }
    bar->addSeparator();
    follow_action_ = bar->addAction(ui_icon(UiIcon::follow), text("follow_live"));
    follow_action_->setCheckable(true);
    follow_action_->setChecked(follow_live_);
    follow_action_->setObjectName("follow_live");
    connect(follow_action_, &QAction::triggered, this, [this](bool enabled) {
        follow_live_ = enabled;
        if (enabled) {
            trigger_armed_ = false;
            update_live_view(true);
            if (changed)
                changed(begin, end, cursor_a, cursor_b);
            update();
        }
    });
    auto *window = bar->addAction(ui_icon(UiIcon::time_window), text("time_span"));
    time_span_edit_ = new QLineEdit;
    normalize_decimal_point(time_span_edit_);
    time_span_edit_->setObjectName("scope_time_span");
    time_span_edit_->setFixedWidth(96);
    time_span_edit_->setText(engineering_value(time_span_, "s"));
    time_span_edit_->setToolTip(text("time_span_hint"));
    time_span_edit_->setAccessibleName(text("time_span"));
    time_span_edit_->setStyleSheet("padding:3px 5px;min-height:22px;");
    bar->addWidget(time_span_edit_);
    connect(window, &QAction::triggered, this, [this] {
        time_span_edit_->setFocus();
        time_span_edit_->selectAll();
    });
    connect(time_span_edit_, &QLineEdit::editingFinished, this, [this] {
        try {
            const double seconds = parse_si(time_span_edit_->text().toStdString(), "s");
            if (!std::isfinite(seconds) || seconds < 0)
                throw std::runtime_error(text("measurement_axis_bounds").toStdString());
            if (seconds != time_span_)
                set_time_span(seconds);
            time_span_edit_->setText(engineering_value(time_span_, "s"));
            time_span_edit_->setStyleSheet("padding:3px 5px;min-height:22px;");
            time_span_edit_->setToolTip(text("time_span_hint"));
        } catch (const std::exception &e) {
            time_span_edit_->setStyleSheet("padding:3px 5px;min-height:22px;border:1px solid #b8394e;");
            time_span_edit_->setToolTip(QString::fromUtf8(e.what()));
        }
    });
    auto *more = new QToolButton;
    more->setIcon(ui_icon(UiIcon::settings));
    more->setIconSize({22, 22});
    more->setAccessibleName(text("ranges"));
    more->setToolTip(text("ranges"));
    more->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(more);
    more->setMenu(menu);
    auto *back = menu->addAction(text("previous_view"));
    back->setObjectName("view_back");
    connect(back, &QAction::triggered, this, [this] { restore_view(-1); });
    auto *forward = menu->addAction(text("next_view"));
    forward->setObjectName("view_forward");
    connect(forward, &QAction::triggered, this, [this] { restore_view(1); });
    auto *measure = bar->addAction(ui_icon(UiIcon::measurements), text("measurements"));
    measure->setObjectName("measurements");
    connect(measure, &QAction::triggered, this, &Scope::show_measurements);
    auto *settings = menu->addAction(text("ranges"));
    settings->setObjectName("scope_settings");
    connect(settings, &QAction::triggered, this, &Scope::show_display_settings);
    auto *snapshot = menu->addAction(text("export_image"));
    connect(snapshot, &QAction::triggered, this, [this] {
        auto path = QFileDialog::getSaveFileName(this, text("export_image"), {}, "PNG (*.png)");
        if (!path.isEmpty() && !grab().save(path, "PNG"))
            QMessageBox::warning(this, text("error"), text("image_write_error"));
    });
    bar->addWidget(more);
    cursor_panel_ = new QWidget;
    cursor_panel_->setObjectName("cursor_panel");
    cursor_panel_->setStyleSheet(
        "QLineEdit,QComboBox{padding:2px 5px;min-height:18px;max-height:22px;}QPushButton{padding:2px "
        "6px;min-height:18px;max-height:22px;}");
    layout->addWidget(cursor_panel_);
    auto *grid = new QGridLayout(cursor_panel_);
    grid->setContentsMargins(0, 2, 0, 2);
    grid->setVerticalSpacing(2);
    auto *cursor_type = new QComboBox;
    cursor_type->setObjectName("cursor_type");
    cursor_type->addItems({text("signal_cursors"), text("screen_cursors")});
    cursor_type->setCurrentIndex(screen_cursors_ ? 1 : 0);
    grid->addWidget(cursor_type, 0, 5);
    connect(cursor_type, &QComboBox::currentIndexChanged, this, [this](int mode) {
        for (int i = 0; i < 2; ++i)
            if (auto r = cursor_reading(i))
                cursor_y_[i] = r->value;
        screen_cursors_ = mode == 1;
        notify_view();
    });
    for (int i = 0; i < 2; ++i) {
        const QString id = i == 0 ? "a" : "b";
        auto *button = new QPushButton(i == 0 ? "A" : "B");
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setMaximumWidth(36);
        cursor_buttons_[i] = button;
        grid->addWidget(button, i, 0);
        connect(button, &QPushButton::clicked, this, [this, i] {
            active_cursor_ = i;
            set_cursor_mode(true);
        });
        auto *choice = new QComboBox;
        choice->setObjectName("cursor_channel_" + id);
        choice->setMinimumContentsLength(12);
        choice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        cursor_choices_[i] = choice;
        grid->addWidget(choice, i, 1);
        connect(choice, &QComboBox::currentIndexChanged, this, [this, i, choice](int) {
            select_cursor_channel(i, choice->currentData().toString().toStdString());
        });
        grid->addWidget(new QLabel("t, s"), i, 2);
        auto *time = new QLineEdit;
        normalize_decimal_point(time);
        time->setObjectName("cursor_time_" + id);
        time->setMaximumWidth(130);
        cursor_times_[i] = time;
        grid->addWidget(time, i, 3);
        connect(time, &QLineEdit::editingFinished, this, [this, i, time] {
            try {
                double t = parse_si(time->text().toStdString(), "s");
                if (t < 0)
                    throw std::runtime_error(text("positive_time").toStdString());
                set_cursor(i, t);
                time->setStyleSheet({});
            } catch (const std::exception &e) {
                time->setStyleSheet("border:1px solid #b8394e");
                time->setToolTip(QString::fromUtf8(e.what()));
            }
        });
        cursor_values_[i] = new QLabel;
        cursor_values_[i]->setObjectName("cursor_value_" + id);
        grid->addWidget(cursor_values_[i], i, 4);
    }
    cursor_buttons_[0]->setChecked(true);
    grid->setColumnStretch(1, 1);
    cursor_math_ = new QLabel;
    cursor_math_->setObjectName("cursor_math");
    cursor_math_->setWordWrap(true);
    cursor_math_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    cursor_math_->setToolTip(text("cursor_frequency_hint"));
    grid->addWidget(cursor_math_, 2, 0, 1, 5);
    auto *clear = new QPushButton(ui_icon(UiIcon::clear), QString());
    clear->setToolTip(text("clear_cursors"));
    clear->setAccessibleName(text("clear_cursors"));
    clear->setFixedWidth(30);
    clear->setObjectName("clear_cursors");
    grid->addWidget(clear, 1, 5);
    connect(clear, &QPushButton::clicked, this, [this] {
        cursor_a = cursor_b = -1;
        active_cursor_ = 0;
        set_cursor_mode(false);
        cursor_panel_->hide();
        notify_view();
    });
    populate_cursor_channels();
    update_cursor_panel();
    set_navigation_tool(navigation_tool_);
    return container;
}
void Scope::set_cursor_mode(bool enabled) {
    set_navigation_tool(enabled ? NavigationTool::cursors : NavigationTool::pan);
}
void Scope::set_navigation_tool(NavigationTool tool) {
    navigation_tool_ = tool;
    cancel_drag();
    const bool enabled = tool == NavigationTool::cursors;
    if (cursor_action_)
        cursor_action_->setChecked(enabled);
    for (int i = 0; i < 3; ++i)
        if (zoom_actions_[i])
            zoom_actions_[i]->setChecked(tool == NavigationTool::zoom && i == static_cast<int>(zoom_axes));
    if (cursor_panel_)
        cursor_panel_->setVisible(enabled);
    if (cursor_buttons_[active_cursor_])
        cursor_buttons_[active_cursor_]->setChecked(true);
    update();
}
int Scope::cursor_channel(int index) const {
    if (!result_)
        return -1;
    for (int channel : channels_)
        if (result_channel(*result_, channel).object == cursor_channels_[index])
            return channel;
    return channels_.empty() ? -1 : channels_.front();
}
void Scope::populate_cursor_channels() {
    for (int i = 0; i < 2; ++i) {
        if (!cursor_choices_[i])
            continue;
        QSignalBlocker block(cursor_choices_[i]);
        auto *choice = cursor_choices_[i].data();
        choice->clear();
        if (result_)
            for (int channel : channels_) {
                auto info = result_channel(*result_, channel);
                choice->addItem(QString::fromStdString(info.name + " [" + info.unit + "]"),
                                QString::fromStdString(info.object));
            }
        int index = choice->findData(QString::fromStdString(cursor_channels_[i]));
        choice->setCurrentIndex(index >= 0 ? index : 0);
        if (choice->count())
            cursor_channels_[i] = choice->currentData().toString().toStdString();
    }
}
void Scope::select_cursor_channel(int index, const std::string &key) {
    if (index < 0 || index > 1)
        return;
    cursor_channels_[index] = key;
    if (changed)
        changed(begin, end, cursor_a, cursor_b);
    populate_cursor_channels();
    update_cursor_panel();
    update();
}
void Scope::set_cursor(int index, double time) {
    if (!result_ || index < 0 || index > 1)
        return;
    auto reading = cursor_value(*result_, cursor_channel(index), time);
    if (!reading)
        return;
    (index == 0 ? cursor_a : cursor_b) = screen_cursors_ ? time : reading->time;
    notify_view();
}
QString Scope::cursor_readout() const {
    if (!result_)
        return text("cursor_place_hint");
    auto a = cursor_reading(0), b = cursor_reading(1);
    if (!a || !b)
        return text("cursor_place_hint");
    const auto m = compare_cursors(*a, *b);
    QString result = "Δt = " + engineering_value(m.dt, "s") +
                     "    |Δt| = " + engineering_value(m.interval, "s") +
                     "    1/|Δt| = " + (m.frequency ? engineering_value(*m.frequency, "Hz") : QString("—"));
    result += "    ΔY = " + (m.dy ? engineering_value(*m.dy, a->unit) : text("different_units"));
    result += "    ΔY/Δt = " + (m.slope ? engineering_value(*m.slope, a->unit + "/s") : QString("—"));
    return result;
}
void Scope::update_cursor_panel() {
    for (int i = 0; i < 2; ++i)
        if (cursor_times_[i]) {
            auto reading = cursor_reading(i);
            if (!cursor_times_[i]->hasFocus())
                cursor_times_[i]->setText(reading ? QString::number(reading->time, 'g', 12) : QString());
            cursor_values_[i]->setText(reading ? engineering_value(reading->value, reading->unit)
                                               : QString("—"));
        }
    if (cursor_math_)
        cursor_math_->setText(cursor_readout());
}
void Scope::notify_view() {
    follow_live_ = false;
    trigger_armed_ = false;
    if (follow_action_)
        follow_action_->setChecked(false);
    if (separate_axes_ && !display_ranges_.empty())
        display_ranges_[active_lane_] = {y_low, y_high};
    if (drag_button_ == Qt::NoButton)
        remember_view();
    if (changed)
        changed(begin, end, cursor_a, cursor_b);
    update_cursor_panel();
    update();
}
void Scope::cancel_drag() {
    if (legend_drag_lane_ >= 0) {
        std::erase_if(legend_positions_,
                      [&](const auto &p) { return p.display == unsigned(legend_drag_lane_); });
        if (legend_drag_original_)
            legend_positions_.push_back(*legend_drag_original_);
        legend_drag_lane_ = -1;
    }
    selecting_zoom_ = false;
    drag_button_ = Qt::NoButton;
    setCursor(navigation_tool_ == NavigationTool::pan ? Qt::OpenHandCursor : Qt::CrossCursor);
    update();
}
void Scope::mousePressEvent(QMouseEvent *e) {
    if (!result_ || !plot_rect().contains(e->position()))
        return;
    if (drag_button_ != Qt::NoButton) {
        if (e->button() == Qt::RightButton) {
            QKeyEvent cancel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            keyPressEvent(&cancel);
        }
        e->accept();
        return;
    }
    if (e->button() == Qt::RightButton && navigation_tool_ != NavigationTool::cursors) {
        cancel_drag();
        return;
    }
    if (e->button() != Qt::LeftButton && e->button() != Qt::MiddleButton && e->button() != Qt::RightButton)
        return;
    if (e->button() == Qt::LeftButton && (legend_drag_lane_ = legend_at(e->position())) >= 0) {
        legend_drag_box_ = legend_layout(legend_drag_lane_).box;
        legend_drag_original_.reset();
        for (const auto &pos : legend_positions_)
            if (pos.display == unsigned(legend_drag_lane_))
                legend_drag_original_ = pos;
        legend_drag_moved_ = false;
        drag_origin_ = e->position();
        drag_button_ = e->button();
        setFocus();
        setCursor(Qt::ClosedHandCursor);
        e->accept();
        return;
    }
    follow_live_ = false;
    setFocus();
    if (!activate_lane(e->position()))
        return;
    remember_view();
    drag_cursors_[0] = cursor_a;
    drag_cursors_[1] = cursor_b;
    drag_cursors_[2] = cursor_y_[0];
    drag_cursors_[3] = cursor_y_[1];
    drag_button_ = e->button();
    drag_origin_ = drag_current_ = e->position();
    drag_begin_ = begin;
    drag_end_ = end;
    drag_low_ = y_low;
    drag_high_ = y_high;
    if (drag_button_ == Qt::MiddleButton || navigation_tool_ == NavigationTool::pan) {
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (navigation_tool_ == NavigationTool::cursors) {
        if (e->button() == Qt::RightButton)
            active_cursor_ = 1;
        else {
            double distance = 9;
            for (int i = 0; i < 2; ++i) {
                double t = i == 0 ? cursor_a : cursor_b;
                if (t < begin || t > end)
                    continue;
                double dx = std::abs(lane_rect(active_lane_).left() +
                                     (t - begin) / (end - begin) * lane_rect(active_lane_).width() -
                                     e->position().x());
                if (dx < distance) {
                    distance = dx;
                    active_cursor_ = i;
                }
            }
        }
        if (cursor_buttons_[active_cursor_])
            cursor_buttons_[active_cursor_]->setChecked(true);
        mouseMoveEvent(e);
    } else {
        selecting_zoom_ = true;
        setCursor(Qt::CrossCursor);
        update();
    }
    e->accept();
}
void Scope::mouseMoveEvent(QMouseEvent *e) {
    if (legend_drag_lane_ >= 0) {
        move_legend(e->position());
        e->accept();
        return;
    }
    if (drag_button_ == Qt::NoButton)
        setCursor(legend_at(e->position()) >= 0 || navigation_tool_ == NavigationTool::pan
                      ? Qt::OpenHandCursor
                      : Qt::CrossCursor);
    if (!result_ || drag_button_ == Qt::NoButton)
        return;
    const auto area = lane_rect(active_lane_);
    drag_current_ = {std::clamp(e->position().x(), area.left(), area.right()),
                     std::clamp(e->position().y(), area.top(), area.bottom())};
    if (selecting_zoom_) {
        update();
        return;
    }
    if (drag_button_ == Qt::MiddleButton || navigation_tool_ == NavigationTool::pan) {
        double dx = (e->position().x() - drag_origin_.x()) / area.width() * (drag_end_ - drag_begin_);
        double dy = (e->position().y() - drag_origin_.y()) / area.height() * (drag_high_ - drag_low_);
        begin = std::max(0., drag_begin_ - dx);
        end = begin + drag_end_ - drag_begin_;
        y_low = drag_low_ + dy;
        y_high = drag_high_ + dy;
        notify_view();
    } else {
        if (screen_cursors_)
            cursor_y_[active_cursor_] =
                y_high - (drag_current_.y() - area.top()) / area.height() * (y_high - y_low);
        set_cursor(active_cursor_, begin + (drag_current_.x() - area.left()) / area.width() * (end - begin));
    }
}
void Scope::mouseReleaseEvent(QMouseEvent *e) {
    if (drag_button_ != e->button())
        return;
    mouseMoveEvent(e);
    if (legend_drag_lane_ >= 0) {
        legend_drag_lane_ = -1;
        cancel_drag();
        if (legend_drag_moved_ && changed)
            changed(begin, end, cursor_a, cursor_b);
        e->accept();
        return;
    }
    if (selecting_zoom_) {
        const auto area = lane_rect(active_lane_);
        const QRectF selection = QRectF(drag_origin_, drag_current_).normalized();
        if ((zoom_axes == Axes::y || selection.width() >= 5) &&
            (zoom_axes == Axes::x || selection.height() >= 5)) {
            if (zoom_axes != Axes::y) {
                begin =
                    drag_begin_ + (selection.left() - area.left()) / area.width() * (drag_end_ - drag_begin_);
                end = drag_begin_ +
                      (selection.right() - area.left()) / area.width() * (drag_end_ - drag_begin_);
            }
            if (zoom_axes != Axes::x) {
                y_low =
                    drag_high_ - (selection.bottom() - area.top()) / area.height() * (drag_high_ - drag_low_);
                y_high =
                    drag_high_ - (selection.top() - area.top()) / area.height() * (drag_high_ - drag_low_);
            }
        }
    } else if (navigation_tool_ == NavigationTool::cursors && drag_button_ != Qt::MiddleButton &&
               active_cursor_ == 0 && cursor_b < 0) {
        active_cursor_ = 1;
        if (cursor_buttons_[1])
            cursor_buttons_[1]->setChecked(true);
    }
    cancel_drag();
    notify_view();
    e->accept();
}
void Scope::wheelEvent(QWheelEvent *e) {
    if (legend_at(e->position()) >= 0) {
        e->accept();
        return;
    }
    if (!result_ || drag_button_ != Qt::NoButton || !plot_rect().contains(e->position()))
        return;
    const auto axes = e->modifiers() == wheel_x_ ? Axes::x : e->modifiers() == wheel_y_ ? Axes::y : Axes::xy;
    if (!activate_lane(e->position()))
        return;
    remember_view();
    const auto area = lane_rect(active_lane_);
    const double factor =
        std::pow(.8, e->pixelDelta().isNull() ? e->angleDelta().y() / 120. : e->pixelDelta().y() / 80.);
    if (axes != Axes::y) {
        double fraction = (e->position().x() - area.left()) / area.width(),
               center = begin + fraction * (end - begin);
        double range =
            std::clamp((end - begin) * factor, 1e-12, std::max(1e-12, result_->samples.back().time) * 10);
        begin = std::max(0., center - fraction * range);
        end = begin + range;
    }
    if (axes != Axes::x) {
        double fraction = 1 - (e->position().y() - area.top()) / area.height(),
               center = y_low + fraction * (y_high - y_low);
        double range = std::clamp((y_high - y_low) * factor, 1e-12, 1e100);
        y_low = center - fraction * range;
        y_high = y_low + range;
    }
    notify_view();
    e->accept();
}
void Scope::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) {
        if (legend_drag_lane_ >= 0) {
            cancel_drag();
            e->accept();
            return;
        }
        if (drag_button_ != Qt::NoButton) {
            begin = drag_begin_;
            end = drag_end_;
            y_low = drag_low_;
            y_high = drag_high_;
            cursor_a = drag_cursors_[0];
            cursor_b = drag_cursors_[1];
            cursor_y_[0] = drag_cursors_[2];
            cursor_y_[1] = drag_cursors_[3];
        }
        cancel_drag();
        notify_view();
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}
bool Scope::event(QEvent *e) {
    if (e->type() == QEvent::ShortcutOverride && static_cast<QKeyEvent *>(e)->key() == Qt::Key_Escape &&
        drag_button_ != Qt::NoButton) {
        e->accept();
        return true;
    }
    if (e->type() == QEvent::WindowDeactivate || e->type() == QEvent::Hide ||
        e->type() == QEvent::UngrabMouse)
        cancel_drag();
    return QWidget::event(e);
}
} // namespace pds::desktop
