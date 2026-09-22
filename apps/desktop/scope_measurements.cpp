#include "apps/desktop/editor.hpp"
#include "apps/desktop/number_input.hpp"
#include "apps/desktop/ui_icons.hpp"
#include "results/integrals.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>
namespace pds::desktop {
ViewOptions Scope::view_options() const {
    ViewOptions v;
    v.display_columns = display_columns_;
    v.signal_displays = signal_displays_;
    v.curve_styles = curve_styles_;
    v.curve_names = curve_names_;
    v.curve_multipliers = curve_multipliers_;
    v.legend_positions = legend_positions_;
    v.hidden_channels.assign(hidden_channels_.begin(), hidden_channels_.end());
    v.y_low = y_low;
    v.y_high = y_high;
    v.manual_y = !follow_live_;
    v.free_cursors = screen_cursors_;
    v.separate_axes = separate_axes_;
    v.grid = show_grid_;
    v.legend = show_legend_;
    v.line_width = line_width_;
    v.time_span = time_span_;
    v.cursor_channel_a = cursor_channels_[0];
    v.cursor_channel_b = cursor_channels_[1];
    v.cursor_y_a = cursor_y_[0];
    v.cursor_y_b = cursor_y_[1];
    return v;
}
void Scope::load_view_options(const ViewOptions &v) {
    if (!result_)
        pending_options_ = v;
    separate_axes_ = v.separate_axes;
    display_columns_ = int(v.display_columns);
    signal_displays_ = v.signal_displays;
    curve_styles_ = v.curve_styles;
    curve_names_ = v.curve_names;
    curve_multipliers_ = v.curve_multipliers;
    legend_positions_ = v.legend_positions;
    hidden_channels_ = {v.hidden_channels.begin(), v.hidden_channels.end()};
    screen_cursors_ = v.free_cursors;
    show_grid_ = v.grid;
    show_legend_ = v.legend;
    line_width_ = v.line_width;
    time_span_ = v.time_span;
    if (time_span_edit_)
        time_span_edit_->setText(engineering_value(time_span_, "s"));
    cursor_channels_[0] = v.cursor_channel_a;
    cursor_channels_[1] = v.cursor_channel_b;
    cursor_y_[0] = v.cursor_y_a;
    cursor_y_[1] = v.cursor_y_b;
    fit_y();
    if (v.manual_y) {
        y_low = v.y_low;
        y_high = v.y_high;
    }
    populate_cursor_channels();
    update_cursor_panel();
    update_channel_controls();
    update();
}

namespace {
QLineEdit *number(QFormLayout *form, const char *key, const QString &label, double value) {
    auto *edit = new QLineEdit(QString::number(value, 'g', 12));
    normalize_decimal_point(edit);
    edit->setObjectName(key);
    form->addRow(label, edit);
    return edit;
}
double value(QWidget *parent, const char *name, const std::string &unit = {}) {
    return parse_si(parent->findChild<QLineEdit *>(name)->text().toStdString(), unit);
}
void table_rows(QTableWidget *table, const std::vector<std::pair<QString, QString>> &rows) {
    table->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        table->setItem(i, 0, new QTableWidgetItem(rows[i].first));
        table->setItem(i, 1, new QTableWidgetItem(rows[i].second));
    }
}
} // namespace
void Scope::set_live(bool live) {
    if (live && !live_) {
        trigger_after_ = 0;
        trigger_time_.reset();
        view_history_.clear();
    }
    live_ = live;
    if (live)
        follow_live_ = true;
    else if (changed)
        changed(begin, end, cursor_a, cursor_b);
    if (follow_action_)
        follow_action_->setChecked(follow_live_);
}
void Scope::update_trigger_controls() {
    const bool active=trigger_armed_||trigger_time_.has_value()||trigger_capture_until_.has_value();
    if(trigger_level_edit_&&!trigger_level_edit_->hasFocus())
        trigger_level_edit_->setText(QString::number(trigger_level_,'g',8));
    if(trigger_position_edit_&&!trigger_position_edit_->hasFocus())
        trigger_position_edit_->setText(QString::number(trigger_position_*100,'g',5)+" %");
    if(trigger_arm_action_)trigger_arm_action_->setEnabled(result_&&!channels_.empty()&&!trigger_armed_);
    if(trigger_stop_action_)trigger_stop_action_->setEnabled(active);
    if(trigger_level_label_action_)trigger_level_label_action_->setVisible(active);
    if(trigger_level_action_)trigger_level_action_->setVisible(active);
    if(trigger_position_action_)trigger_position_action_->setVisible(active);
}
void Scope::arm_trigger() {
    if(!result_||channels_.empty())return;
    const bool available=std::any_of(channels_.begin(),channels_.end(),[&](int channel){
        return result_channel(*result_,channel).object==trigger_channel_;
    });
    if(!available)trigger_channel_=result_channel(*result_,channels_.front()).object;
    trigger_after_=live_&&!result_->samples.empty()?result_->samples.back().time:-1;
    trigger_time_.reset();
    trigger_capture_until_.reset();
    trigger_armed_=true;
    if(trigger_mode_==1) {
        follow_live_=false;
        if(follow_action_)follow_action_->setChecked(false);
    }
    update_live_view();
    update_measurements();
    update_trigger_controls();
    update();
}
void Scope::stop_trigger() {
    trigger_armed_=false;
    trigger_time_.reset();
    trigger_capture_until_.reset();
    update_measurements();
    update_trigger_controls();
    update();
}
void Scope::set_time_span(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0)
        return;
    cancel_drag();
    time_span_ = seconds;
    if (pending_options_)
        pending_options_->time_span = seconds;
    if (time_span_edit_)
        time_span_edit_->setText(engineering_value(time_span_, "s"));
    follow_live_ = true;
    trigger_armed_ = false;
    trigger_time_.reset();
    trigger_capture_until_.reset();
    if (follow_action_)
        follow_action_->setChecked(true);
    update_live_view(true);
    if (changed)
        changed(begin, end, cursor_a, cursor_b);
    update();
}
void Scope::update_live_view(bool force) {
    if (!result_) {
        update_trigger_controls();
        return;
    }
    const double latest_time=result_->samples.empty()?0:result_->samples.back().time;
    if(trigger_capture_until_&&latest_time>=*trigger_capture_until_)
        trigger_capture_until_.reset();
    if (trigger_armed_&&!trigger_capture_until_) {
        for (int ch : channels_)
            if (result_channel(*result_, ch).object == trigger_channel_) {
                auto edges =
                    signal_edges(*result_, ch, trigger_after_, result_->samples.back().time, trigger_level_);
                bool triggered = false;
                for (auto edge : edges)
                    if ((trigger_edge_ == 2 || edge.rising == (trigger_edge_ == 0)) &&
                        edge.time > trigger_after_ &&
                        (!trigger_time_ || edge.time >= *trigger_time_ + trigger_holdoff_)) {
                        trigger_time_ = edge.time;
                        trigger_armed_ = trigger_mode_ != 0;
                        triggered = true;
                        const double span = time_span_ > 0 ? time_span_ : std::max(1e-6, end - begin);
                        trigger_capture_until_=edge.time+span*(1-trigger_position_);
                        if (trigger_mode_ == 0)
                            break;
                    }
                if (triggered) {
                    const double span = time_span_ > 0 ? time_span_ : std::max(1e-6, end - begin);
                    begin = std::max(0., *trigger_time_ - span * trigger_position_);
                    end = begin + span;
                    follow_live_ = false;
                    if (follow_action_)
                        follow_action_->setChecked(false);
                    fit_y();
                    if (changed)
                        changed(begin, end, cursor_a, cursor_b);
                }
                trigger_after_ = result_->samples.back().time;
                break;
            }
    }
    if (live_ && trigger_armed_ && trigger_mode_ == 2 && !trigger_capture_until_ &&
        (!trigger_time_ || result_->samples.back().time - *trigger_time_ > std::max(time_span_, end - begin)))
        follow_live_ = true;
    if ((live_ || force) && follow_live_) {
        end = std::max({1e-12, time_span_, result_->samples.back().time});
        begin = time_span_ > 0 ? std::max(0., end - time_span_) : 0;
        fit_y();
    }
    if (follow_action_)
        follow_action_->setChecked(follow_live_);
    update_trigger_controls();
}
void Scope::remember_view() {
    if (separate_axes_ && !display_ranges_.empty())
        display_ranges_[active_lane_] = {y_low, y_high};
    ViewRange next{begin, end, y_low, y_high, display_ranges_};
    if (!view_history_.empty()) {
        const auto &last = view_history_[view_index_];
        if (last.begin == begin && last.end == end && last.low == y_low && last.high == y_high &&
            last.lanes == display_ranges_)
            return;
        view_history_.resize(view_index_ + 1);
    }
    view_history_.push_back(next);
    if (view_history_.size() > 100)
        view_history_.erase(view_history_.begin());
    view_index_ = view_history_.size() - 1;
}
void Scope::restore_view(int direction) {
    if (view_history_.empty() || (direction < 0 && view_index_ == 0) ||
        (direction > 0 && view_index_ + 1 == view_history_.size()))
        return;
    view_index_ = static_cast<size_t>(static_cast<int>(view_index_) + direction);
    const auto &v = view_history_[view_index_];
    begin = v.begin;
    end = v.end;
    y_low = v.low;
    y_high = v.high;
    display_ranges_ = v.lanes;
    follow_live_ = false;
    if (follow_action_)
        follow_action_->setChecked(false);
    if (changed)
        changed(begin, end, cursor_a, cursor_b);
    update_cursor_panel();
    update();
}
std::optional<CursorValue> Scope::cursor_reading(int index) const {
    if (!result_)
        return {};
    const double t = index == 0 ? cursor_a : cursor_b;
    if (screen_cursors_) {
        if (t < 0 || cursor_channel(index) < 0)
            return {};
        const auto channel = result_channel(*result_, cursor_channel(index));
        return CursorValue{t, cursor_y_[index], channel.object, channel.name, channel.unit};
    }
    return cursor_value(*result_, cursor_channel(index), t);
}
int Scope::channel_display(int channel) const {
    if (!separate_axes_ || !result_)
        return 0;
    auto key = result_channel(*result_, channel).object;
    for (const auto &[id, display] : signal_displays_)
        if (id == key)
            return int(display);
    auto it = std::find(channels_.begin(), channels_.end(), channel);
    return std::clamp(int(it - channels_.begin()), 0, 15);
}
int Scope::display_count() const {
    int count = 1;
    if (separate_axes_)
        for (int channel : channels_)
            count = std::max(count, channel_display(channel) + 1);
    return count;
}
QRectF Scope::lane_rect(int lane) const {
    auto area = plot_rect();
    if (!separate_axes_)
        return area;
    const int columns = std::min(display_columns_, display_count()),
              rows = (display_count() + columns - 1) / columns;
    double dx = (area.width() + 58) / columns, dy = (area.height() + 36) / rows;
    return {area.left() + dx * (lane % columns), area.top() + dy * (lane / columns), std::max(1., dx - 58),
            std::max(1., dy - 36)};
}
bool Scope::activate_lane(QPointF point) {
    if (!separate_axes_ || display_ranges_.empty())
        return plot_rect().contains(point);
    for (int lane = 0; lane < display_count(); ++lane)
        if (lane_rect(lane).contains(point)) {
            active_lane_ = lane;
            y_low = display_ranges_[lane].first;
            y_high = display_ranges_[lane].second;
            return true;
        }
    return false;
}
void Scope::show_measurements() {
    if (measurements_) {
        measurements_->show();
        measurements_->raise();
        return;
    }
    auto *dialog = new QDialog(this);
    measurements_ = dialog;
    dialog->setObjectName("scope_measurements");
    dialog->setWindowTitle(text("measurements"));
    dialog->resize(560, 570);
    auto *layout = new QVBoxLayout(dialog);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *signal = new QComboBox;
    signal->setObjectName("measurement_channel");
    form->addRow(text("measurement_signal"), signal);
    if (result_)
        for (int ch : channels_) {
            auto c = result_channel(*result_, ch);
            signal->addItem(QString::fromStdString(c.name + " [" + c.unit + "]"), ch);
        }
    auto *range = new QComboBox;
    range->setObjectName("measurement_range");
    range->addItems({text("visible_range"), text("cursor_range"), text("full_range")});
    form->addRow(text("measurement_range"), range);
    auto *tabs = new QTabWidget;
    tabs->setObjectName("measurement_tabs");
    layout->addWidget(tabs, 1);
    for (const char *name : {"statistics", "peaks", "pulse"}) {
        auto *page = new QWidget;
        auto *box = new QVBoxLayout(page);
        auto *options = new QFormLayout;
        box->addLayout(options);
        if (QString(name) == "statistics") {
            auto *weighting = new QComboBox;
            weighting->setObjectName("measurement_weighting");
            weighting->addItems({text("weight_time"), text("weight_samples")});
            options->addRow(text("weighting"), weighting);
            connect(weighting, &QComboBox::currentIndexChanged, this, [this] { update_measurements(); });
        }
        if (QString(name) == "peaks") {
            number(options, "peak_threshold", text("peak_threshold"), -1e100);
            number(options, "peak_excursion", text("peak_excursion"), 0);
            number(options, "peak_distance", text("peak_distance"), 0);
        }
        if (QString(name) == "pulse") {
            auto *automatic = new QCheckBox(text("auto_levels"));
            automatic->setObjectName("auto_levels");
            automatic->setChecked(true);
            options->addRow(automatic);
            number(options, "low_level", text("low_level"), 0);
            number(options, "high_level", text("high_level"), 1);
            number(options, "lower_reference", text("measurement_lower_reference"), 10);
            number(options, "upper_reference", text("measurement_upper_reference"), 90);
        }
        auto *table = new QTableWidget(0, 2);
        table->setObjectName(QString("measurement_") + name);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table->verticalHeader()->hide();
        table->horizontalHeader()->hide();
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        box->addWidget(table, 1);
        tabs->addTab(page, text(name));
    }
    auto *trigger = new QWidget;
    trigger->setObjectName("trigger_page");
    auto *tf = new QFormLayout(trigger);
    tabs->addTab(trigger, text("trigger"));
    auto *trigger_signal = new QComboBox;
    trigger_signal->setObjectName("trigger_channel");
    if (result_)
        for (int ch : channels_) {
            const auto channel = result_channel(*result_, ch);
            trigger_signal->addItem(QString::fromStdString(channel.name + " [" + channel.unit + "]"), ch);
            if (channel.object == trigger_channel_)
                trigger_signal->setCurrentIndex(trigger_signal->count() - 1);
        }
    tf->addRow(text("measurement_signal"), trigger_signal);
    number(tf, "trigger_level", text("level"), trigger_level_);
    auto *edge = new QComboBox;
    edge->addItems({text("rising"), text("falling"), text("either_edge")});
    edge->setCurrentIndex(trigger_edge_);
    edge->setObjectName("trigger_edge");
    tf->addRow(text("trigger"), edge);
    auto *mode = new QComboBox;
    mode->setObjectName("trigger_mode");
    mode->addItem(text("trigger_normal"), 1);
    mode->addItem(text("trigger_auto"), 2);
    mode->addItem(text("trigger_single"), 0);
    mode->setCurrentIndex(std::max(0, mode->findData(trigger_mode_)));
    tf->addRow(text("trigger_mode"), mode);
    number(tf, "trigger_holdoff", text("trigger_holdoff"), trigger_holdoff_);
    number(tf, "trigger_position", text("trigger_position"), trigger_position_ * 100);
    auto *arm = new QPushButton(text("apply"));
    auto *reset = new QPushButton(text("trigger_reset"));
    tf->addRow(arm, reset);
    auto *status = new QLabel;
    status->setObjectName("trigger_status");
    tf->addRow(status);
    connect(arm, &QPushButton::clicked, this, [this, trigger_signal, edge, mode, dialog, status] {
        try {
            if (!result_ || trigger_signal->currentIndex() < 0)
                return;
            double level = value(dialog, "trigger_level"), holdoff = value(dialog, "trigger_holdoff", "s"),
                   position = value(dialog, "trigger_position") / 100;
            if (holdoff < 0 || position < 0 || position > 1)
                throw std::runtime_error(text("trigger_bounds").toStdString());
            trigger_level_ = level;
            trigger_holdoff_ = holdoff;
            trigger_position_ = position;
            trigger_mode_ = mode->currentData().toInt();
            trigger_channel_ = result_channel(*result_, trigger_signal->currentData().toInt()).object;
            trigger_edge_ = edge->currentIndex();
            update_trigger_controls();
            update();
        } catch (const std::exception &e) {
            status->setText(QString::fromUtf8(e.what()));
        }
    });
    connect(reset, &QPushButton::clicked, this, [this] {
        stop_trigger();
    });
    auto *energy = new QWidget;
    auto *energy_box = new QVBoxLayout(energy);
    auto *energy_form = new QFormLayout;
    energy_box->addLayout(energy_form);
    auto *current = new QComboBox;
    current->setObjectName("measurement_current");
    if (result_)
        for (int ch : channels_) {
            const auto c = result_channel(*result_, ch);
            if (c.unit == "A")
                current->addItem(QString::fromStdString(c.name + " [A]"), ch);
        }
    energy_form->addRow(text("energy_current"), current);
    auto *reverse = new QCheckBox(text("energy_reverse"));
    reverse->setObjectName("measurement_reverse_current");
    energy_form->addRow(reverse);
    auto *energy_hint = new QLabel(text("energy_hint"));
    energy_hint->setWordWrap(true);
    energy_box->addWidget(energy_hint);
    auto *energy_table = new QTableWidget(0, 2);
    energy_table->setObjectName("measurement_energy");
    energy_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    energy_table->horizontalHeader()->hide();
    energy_table->verticalHeader()->hide();
    energy_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    energy_box->addWidget(energy_table, 1);
    tabs->addTab(energy, text("energy"));
    connect(current, &QComboBox::currentIndexChanged, this, [this] { update_measurements(); });
    connect(reverse, &QCheckBox::toggled, this, [this] { update_measurements(); });
    auto *hint = new QLabel(text("measure_hint"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *refresh = new QPushButton(text("refresh_measurements"));
    layout->addWidget(refresh);
    connect(refresh, &QPushButton::clicked, this, [this, dialog] {
        dialog->setProperty("measurement_signature", QVariant());
        update_measurements();
    });
    connect(tabs, &QTabWidget::currentChanged, this, [this] { update_measurements(); });
    connect(signal, &QComboBox::currentIndexChanged, this, [this] { update_measurements(); });
    connect(range, &QComboBox::currentIndexChanged, this, [this] { update_measurements(); });
    auto *timer = new QTimer(dialog);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, this, [this] {
        if (measurements_ && measurements_->isVisible())
            update_measurements();
    });
    timer->start();
    dialog->show();
    update_measurements();
}
void Scope::update_measurements() {
    if (!measurements_ || !result_ || result_->samples.empty())
        return;
    auto *dialog = measurements_.data();
    auto *choice = dialog->findChild<QComboBox *>("measurement_channel");
    const int ch = choice->currentData().toInt();
    if (choice->currentIndex() < 0 || ch < 0 ||
        ch >= int(result_->channels.size() + result_->gate_objects.size()))
        return;
    auto *status = dialog->findChild<QLabel *>("trigger_status");
    status->setText((trigger_time_ ? text("trigger_hit").arg(*trigger_time_, 0, 'g', 12) : QString()) +
                    (trigger_armed_ ? "  " + text("trigger_wait") : QString()));
    const int tab = dialog->findChild<QTabWidget *>("measurement_tabs")->currentIndex();
    if (tab == 3)
        return;
    const auto unit = result_channel(*result_, ch).unit;
    double a = begin, b = end;
    int range = dialog->findChild<QComboBox *>("measurement_range")->currentIndex();
    if (range == 1) {
        a = std::min(cursor_a, cursor_b);
        b = std::max(cursor_a, cursor_b);
        if (a < 0)
            b = a = -1;
    }
    if (range == 2) {
        a = 0;
        b = result_->samples.back().time;
    }
    QVariantList signature{tab, ch, a, b, qulonglong(result_->samples.size()), result_->samples.back().time};
    for (auto *edit : dialog->findChildren<QLineEdit *>())
        signature.push_back(edit->text());
    for (auto *combo : dialog->findChildren<QComboBox *>())
        signature.push_back(combo->currentIndex());
    for (auto *check : dialog->findChildren<QCheckBox *>())
        signature.push_back(check->isChecked());
    if (dialog->property("measurement_signature").toList() == signature)
        return;
    dialog->setProperty("measurement_signature", signature);
    auto *table = dialog->findChild<QTableWidget *>(tab == 0   ? "measurement_statistics"
                                                    : tab == 1 ? "measurement_peaks"
                                                    : tab == 2 ? "measurement_pulse"
                                                               : "measurement_energy");
    auto v = [&](double x) { return engineering_value(x, unit); };
    auto t = [](double x) { return engineering_value(x, "s"); };
    try {
        if (tab == 0) {
            auto s = signal_statistics(*result_, ch, a, b);
            const bool by_time = dialog->findChild<QComboBox *>("measurement_weighting")->currentIndex() == 0;
            const auto weighted = by_time ? time_statistics(*result_, ch, a, b) : TimeStatistics{};
            if (!s.count && !weighted.intervals) {
                table_rows(table, {});
                return;
            }
            std::vector<std::pair<QString, QString>> rows;
            if (s.count)
                rows = {{"N", QString::number(s.count)},
                        {text("measurement_minimum"), v(s.minimum) + " @ " + t(s.minimum_time)},
                        {text("measurement_maximum"), v(s.maximum) + " @ " + t(s.maximum_time)},
                        {text("measurement_peak_to_peak"), v(s.maximum - s.minimum)},
                        {text("measurement_sample_median"), v(s.median)}};
            if (!by_time || weighted.intervals) {
                const double mean = by_time ? weighted.mean : s.mean;
                const double rms = by_time ? weighted.rms : s.rms;
                const double deviation = by_time ? weighted.standard_deviation : s.standard_deviation;
                rows.insert(rows.end(), {{text("measurement_mean"), v(mean)}, {"RMS", v(rms)},
                    {text("measurement_mean_square"), engineering_value(rms * rms, unit + "²")},
                    {text("measurement_standard_deviation"), v(deviation)}});
                if (by_time) {
                    rows.emplace_back(text("measurement_duration"), t(weighted.end - weighted.begin));
                    rows.emplace_back(text("measurement_integral"), engineering_value(weighted.integral, unit + "·s"));
                }
            }
            table_rows(table, rows);
        } else if (tab == 1) {
            const double excursion = value(dialog, "peak_excursion"),
                         distance = value(dialog, "peak_distance", "s");
            if (excursion < 0 || distance < 0)
                throw std::runtime_error(text("measurement_peak_bounds").toStdString());
            auto peaks =
                signal_peaks(*result_, ch, a, b, value(dialog, "peak_threshold"), excursion, distance, 100);
            std::vector<std::pair<QString, QString>> rows;
            for (auto peak : peaks)
                rows.emplace_back(t(peak.time), v(peak.value));
            table_rows(table, rows);
        } else if (tab == 4) {
            auto *current = dialog->findChild<QComboBox *>("measurement_current");
            if (unit != "V" || current->currentIndex() < 0) {
                table_rows(table, {{text("energy"), text("energy_choose_channels")}});
                return;
            }
            const auto p = power_energy(*result_, ch, current->currentData().toInt(), a, b,
                dialog->findChild<QCheckBox *>("measurement_reverse_current")->isChecked());
            if (!p.intervals) {
                table_rows(table, {});
                return;
            }
            table_rows(table, {{text("energy_net"), engineering_value(p.energy, "J")},
                {text("energy_absorbed"), engineering_value(p.absorbed, "J")},
                {text("energy_returned"), engineering_value(p.returned, "J")},
                {text("energy_mean_power"), engineering_value(p.mean_power, "W")},
                {text("energy_min_power"), engineering_value(p.minimum_power, "W")},
                {text("energy_max_power"), engineering_value(p.maximum_power, "W")},
                {text("measurement_duration"), t(p.end - p.begin)},
                {text("measurement_intervals"), QString::number(p.intervals)}});
        } else {
            std::optional<std::pair<double, double>> levels;
            if (!dialog->findChild<QCheckBox *>("auto_levels")->isChecked())
                levels = {{value(dialog, "low_level"), value(dialog, "high_level")}};
            double lower = value(dialog, "lower_reference") / 100,
                   upper = value(dialog, "upper_reference") / 100;
            if ((levels && levels->first >= levels->second) || lower < 0 || upper > 1 || lower >= upper)
                throw std::runtime_error(text("measurement_reference_bounds").toStdString());
            auto m = pulse_measurements(*result_, ch, a, b, levels, lower, upper);
            auto optional = [](auto value, const std::string &unit) {
                return value ? engineering_value(*value, unit) : QString("—");
            };
            table_rows(table,
                       {{text("measurement_low"), v(m.low)},
                        {text("measurement_high"), v(m.high)},
                        {text("measurement_cycles"), QString::number(m.cycles)},
                        {text("measurement_period"), optional(m.period, "s")},
                        {text("measurement_frequency"), optional(m.frequency, "Hz")},
                        {text("measurement_duty"), optional(m.duty, "%")},
                        {text("measurement_high_width"), optional(m.high_width, "s")},
                        {text("measurement_low_width"), optional(m.low_width, "s")},
                        {text("measurement_rise_time"), optional(m.rise_time, "s")},
                        {text("measurement_fall_time"), optional(m.fall_time, "s")},
                        {text("measurement_overshoot"), QString::number(m.overshoot, 'g', 8) + " %"},
                        {text("measurement_undershoot"), QString::number(m.undershoot, 'g', 8) + " %"}});
        }
    } catch (const std::exception &e) {
        table_rows(table, {{text("error"), QString::fromUtf8(e.what())}});
    }
}
void Scope::show_display_settings() {
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName("scope_display");
    dialog->setWindowTitle(text("ranges"));
    auto *form = new QFormLayout(dialog);
    number(form, "x_min", "X min, s", begin);
    number(form, "x_max", "X max, s", end);
    number(form, "y_min", "Y min", y_low);
    number(form, "y_max", "Y max", y_high);
    number(form, "time_span", text("time_span"), time_span_);
    number(form, "line_width", text("line_width"), line_width_);
    auto *separate = new QCheckBox(text("separate_axes"));
    separate->setObjectName("separate_axes");
    separate->setChecked(separate_axes_);
    form->addRow(separate);
    auto *columns = new QSpinBox;
    columns->setRange(1, 4);
    columns->setValue(display_columns_);
    columns->setObjectName("display_columns");
    form->addRow(text("display_columns"), columns);
    auto *bindings = new QTableWidget(int(channels_.size()), 3);
    bindings->setObjectName("display_bindings");
    bindings->setHorizontalHeaderLabels({text("signal_cursors"), text("display"), text("line_type")});
    bindings->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    bindings->setMaximumHeight(200);
    bindings->verticalHeader()->hide();
    for (int row = 0; row < int(channels_.size()); ++row) {
        auto info = result_channel(*result_, channels_[row]);
        auto *name = new QTableWidgetItem(QString::fromStdString(info.name));
        name->setData(Qt::UserRole, QString::fromStdString(info.object));
        name->setFlags(Qt::ItemIsEnabled);
        bindings->setItem(row, 0, name);
        auto *display = new QSpinBox;
        display->setRange(1, 16);
        display->setValue(separate_axes_ ? channel_display(channels_[row]) + 1 : std::min(row + 1, 16));
        bindings->setCellWidget(row, 1, display);
        auto *style = new QToolButton;
        style->setIcon(ui_icon(UiIcon::gear));
        style->setToolTip(text("curve_style").arg(QString::fromStdString(info.name)));
        connect(style, &QToolButton::clicked, this, [this, key = info.object] { show_curve_settings(key); });
        bindings->setCellWidget(row, 2, style);
    }
    form->addRow(bindings);
    auto *grid = new QCheckBox(text("grid"));
    grid->setChecked(show_grid_);
    form->addRow(grid);
    auto *legend = new QCheckBox(text("legend"));
    legend->setChecked(show_legend_);
    form->addRow(legend);
    auto *error = new QLabel;
    error->setWordWrap(true);
    form->addRow(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [=, this] {
        try {
            const double a = value(dialog, "x_min", "s"), b = value(dialog, "x_max", "s"),
                         low = value(dialog, "y_min"), high = value(dialog, "y_max"),
                         span = value(dialog, "time_span", "s"), width = value(dialog, "line_width");
            if (a < 0 || a >= b || low >= high || span < 0 || width <= 0 || width > 10)
                throw std::runtime_error(text("measurement_axis_bounds").toStdString());
            remember_view();
            begin = a;
            end = b;
            y_low = low;
            y_high = high;
            const bool span_changed = time_span_ != span;
            line_width_ = width;
            show_grid_ = grid->isChecked();
            show_legend_ = legend->isChecked();
            std::vector<std::pair<std::string, unsigned>> next_bindings;
            for (int row = 0; row < bindings->rowCount(); ++row)
                next_bindings.emplace_back(
                    bindings->item(row, 0)->data(Qt::UserRole).toString().toStdString(),
                    unsigned(qobject_cast<QSpinBox *>(bindings->cellWidget(row, 1))->value() - 1));
            if (separate_axes_ != separate->isChecked() || next_bindings != signal_displays_ ||
                columns->value() != display_columns_) {
                display_columns_ = columns->value();
                signal_displays_ = std::move(next_bindings);
                separate_axes_ = separate->isChecked();
                active_lane_ = 0;
                fit_y();
                view_history_.clear();
                view_index_ = 0;
            }
            notify_view();
            if (span_changed)
                set_time_span(span);
            error->clear();
        } catch (const std::exception &e) {
            error->setText(QString::fromUtf8(e.what()));
        }
    });
    dialog->show();
}
} // namespace pds::desktop
