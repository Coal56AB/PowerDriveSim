#include "apps/desktop/editor.hpp"
#include "apps/desktop/scope_style.hpp"
#include "apps/desktop/theme.hpp"
#include "results/display_sampling.hpp"
#include "results/measurements.hpp"
#include <QActionGroup>
#include <QApplication>
#include <QBitArray>
#include <QContextMenuEvent>
#include <QDialog>
#include <QFile>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QJsonDocument>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QToolBar>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>
static void init_resources() {
    Q_INIT_RESOURCE(translations);
}
namespace pds::desktop {
static QJsonObject strings;
void init_language(const QString &language) {
    init_resources();
    QFile file(":/i18n/" + (language == "en" ? QString("en") : QString("ru")) + ".json");
    file.open(QIODevice::ReadOnly);
    strings = QJsonDocument::fromJson(file.readAll()).object();
}
QString text(const char *key) {
    return strings.value(key).toString(QString::fromUtf8(key));
}
QString engineering_value(double value, const std::string &unit) {
    if (value == 0)
        return "0 " + QString::fromStdString(unit);
    const std::pair<double, const char *> scales[] = {{1e9, "G"},  {1e6, "M"},  {1e3, "k"},  {1, ""},
                                                      {1e-3, "m"}, {1e-6, "µ"}, {1e-9, "n"}, {1e-12, "p"}};
    for (auto [scale, prefix] : scales)
        if (std::abs(value) >= scale * (1 - 1e-12))
            return QString::number(value / scale, 'g', 8) + " " + QString::fromUtf8(prefix) +
                   QString::fromStdString(unit);
    return QString::number(value, 'g', 8) + " " + QString::fromStdString(unit);
}
Scope::Scope(QWidget *parent, Domain domain) : QWidget(parent), domain_(domain) {
    setMouseTracking(true);
    setMinimumHeight(80);
    setFocusPolicy(Qt::StrongFocus);
    setObjectName("scope");
}
void Scope::set_result(const Result *result, const std::vector<int> &channels, const Project &p) {
    const bool result_changed = result_ != result;
    const bool channels_changed = channels_ != channels;
    const bool new_data = result_changed || channels_changed;
    // A recording checkbox only changes the displayed channel set. Do not
    // discard and rebuild an extrema index over millions of existing samples.
    if (result_changed || (!live_ && !channels_changed))
        extrema_.clear();
    if (result_changed)
        preview_channels_.clear();
    else if (channels_changed) {
        for (int channel : channels)
            if (std::find(channels_.begin(), channels_.end(), channel) == channels_.end() &&
                !extrema_.contains(channel))
                preview_channels_.insert(channel);
        std::erase_if(preview_channels_, [&](int channel) {
            return std::find(channels.begin(), channels.end(), channel) == channels.end();
        });
    }
    result_ = result && !result->samples.empty() ? result : nullptr;
    if (new_data)
        cancel_drag();
    channels_ = channels;
    if (measurements_)
        measurements_->setProperty("measurement_signature", QVariant());
    if (new_data || !live_) {
        begin = p.scope_begin;
        end = p.scope_end;
    }
    cursor_a = p.cursor_a;
    cursor_b = p.cursor_b;
    update_live_view();
    if (result_ && end <= begin) {
        begin = 0;
        end = std::max(1e-12, result_->samples.back().time);
    }
    if (result_changed) {
        fit_y();
        view_history_.clear();
        remember_view();
    }
    if (new_data) {
        populate_cursor_channels();
        update_channel_controls();
        if (measurements_) {
            delete measurements_.data();
            measurements_ = nullptr;
        }
    }
    if (result_ && pending_options_) {
        auto saved = *pending_options_;
        pending_options_.reset();
        load_view_options(saved);
    }
    update_cursor_panel();
    update();
}
void Scope::fit(Axes axes) {
    if (!result_)
        return;
    remember_view();
    if (axes != Axes::y) {
        begin = 0;
        end = std::max(1e-12, result_->samples.back().time);
    }
    if (axes != Axes::x)
        fit_y();
    notify_view();
}
void Scope::fit_y() {
    // Checkbox changes use a bounded preview. A real Y fit opts into the exact
    // extrema pass over the complete recorded history.
    preview_channels_.clear();
    if (!result_ || channels_.empty()) {
        y_low = -1;
        y_high = 1;
        return;
    }
    auto first = std::lower_bound(result_->samples.begin(), result_->samples.end(), begin,
                                  [](const Sample &s, double t) { return s.time < t; });
    auto last = std::upper_bound(result_->samples.begin(), result_->samples.end(), end,
                                 [](double t, const Sample &s) { return t < s.time; });
    const size_t from = static_cast<size_t>(first - result_->samples.begin()),
                 to = static_cast<size_t>(last - result_->samples.begin());
    display_ranges_.assign(
        display_count(), {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()});
    double all_low = std::numeric_limits<double>::infinity(), all_high = -all_low;
    auto padded = [](double low, double high) {
        if (!std::isfinite(low) || !std::isfinite(high)) {
            low = -1;
            high = 1;
        }
        double margin = high == low ? std::max(.1, std::abs(high) * .1) : std::max(1e-6, (high - low) * .08);
        return std::pair{low - margin, high + margin};
    };
    for (int channel : channels_) {
        if (!channel_visible(result_channel(*result_, channel).object))
            continue;
        double low = std::numeric_limits<double>::infinity(), high = -low;
        if (from < to) {
            auto [imin, imax] = sample_extrema(channel, from, to);
            low = sample_value(imin, channel);
            high = sample_value(imax, channel);
        }
        all_low = std::min(all_low, low);
        all_high = std::max(all_high, high);
        auto &range = display_ranges_[channel_display(channel)];
        range.first = std::min(range.first, low);
        range.second = std::max(range.second, high);
    }
    for (auto &range : display_ranges_)
        range = padded(range.first, range.second);
    active_lane_ = std::clamp(active_lane_, 0, int(display_ranges_.size()) - 1);
    auto range = separate_axes_ ? display_ranges_[active_lane_] : padded(all_low, all_high);
    y_low = range.first;
    y_high = range.second;
}
double Scope::sample_value(size_t sample, int channel) const {
    const auto key = result_channel(*result_, channel).object;
    return channel_value(*result_, sample, channel) * curve_multiplier(key);
}
std::pair<size_t, size_t> Scope::sample_extrema(int channel, size_t from, size_t to) {
    auto value = [&](size_t i) { return sample_value(i, channel); };
    if (live_ && to > from) {
        // Live painting must stay bounded even when the solver produces millions
        // of samples between GUI frames. The complete history remains untouched;
        // the exact append-only extrema index is built when the run finishes.
        constexpr size_t live_budget = 4096;
        const size_t stride = std::max<size_t>(1, (to - from) / live_budget);
        size_t low = from, high = from;
        for (size_t i = from; i < to; i += stride) {
            if (value(i) < value(low)) low = i;
            if (value(i) > value(high)) high = i;
        }
        const size_t last = to - 1;
        if (value(last) < value(low)) low = last;
        if (value(last) > value(high)) high = last;
        return {low, high};
    }
    auto &index = extrema_[channel];
    index.append(result_->samples.size(), value);
    return index.range(from, to, value);
}
void Scope::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), theme_colors().surface);
    const int lanes = display_count();
    for (int lane = 0; lane < lanes; ++lane) {
        const auto plot = lane_rect(lane);
        painter.setPen(theme_colors().muted);
        painter.drawRect(plot);
        if (!result_ || channels_.empty()) {
            painter.drawText(plot, Qt::AlignCenter, text("scope_empty"));
            return;
        }
        if (std::none_of(channels_.begin(), channels_.end(), [this](int channel) {
                return channel_visible(result_channel(*result_, channel).object);
            })) {
            painter.drawText(plot, Qt::AlignCenter, text("all_signals_hidden"));
            return;
        }
        const double low = separate_axes_ ? display_ranges_[lane].first : y_low,
                     high = separate_axes_ ? display_ranges_[lane].second : y_high;
        for (int grid = 0; grid <= 4; ++grid) {
            double fraction = grid / 4.0;
            painter.setPen(theme_colors().grid);
            if (show_grid_)
                painter.drawLine(QPointF(plot.left(), plot.top() + fraction * plot.height()),
                                 QPointF(plot.right(), plot.top() + fraction * plot.height()));
            painter.setPen(theme_colors().muted);
            painter.drawText(QRectF(plot.left() - 58, plot.top() + fraction * plot.height() - 9, 51, 20),
                             Qt::AlignRight, QString::number(high - fraction * (high - low), 'g', 4));
            painter.drawText(QRectF(plot.left() + fraction * plot.width() - 38, plot.bottom() + 5, 76, 20),
                             Qt::AlignCenter, QString::number((begin + fraction * (end - begin)), 'g', 4));
        }
        if (lane + std::min(display_columns_, display_count()) >= lanes)
            painter.drawText(QRectF(plot.left(), height() - 22, plot.width(), 18), Qt::AlignCenter,
                             text(domain_ == Domain::time ? "time_s" : "frequency_hz"));
        const auto &colors = theme_colors().curves;
        painter.save();
        painter.setClipRect(plot);
        auto x = [&](double t) { return plot.left() + (t - begin) / (end - begin) * plot.width(); };
        for (size_t ch = 0; ch < channels_.size(); ++ch) {
            if (channel_display(channels_[ch]) != lane ||
                !channel_visible(result_channel(*result_, channels_[ch]).object))
                continue;
            QPainterPath path;
            bool started = false;
            // Preserve extrema in each display bucket; the CSV always contains every sample.
            const auto &samples = result_->samples;
            auto first = std::lower_bound(samples.begin(), samples.end(), begin,
                                          [](const Sample &s, double t) { return s.time < t; });
            size_t from = static_cast<size_t>(first - samples.begin());
            if (from)
                --from;
            auto last = std::upper_bound(samples.begin(), samples.end(), end,
                                         [](double t, const Sample &s) { return t < s.time; });
            size_t to = std::min(samples.size(), static_cast<size_t>(last - samples.begin()) + 1);
            const size_t stride =
                std::max<size_t>(1, (to - from) / static_cast<size_t>(std::max(1, width() * 2)));
            bool digital = channels_[ch] >= static_cast<int>(result_->channels.size());
            const auto style = curve_style(result_channel(*result_, channels_[ch]).object);
            if (style.marker != CurveMarker::none) {
                const double scale = devicePixelRatioF();
                const int columns = int(plot.width() * scale) + 1, rows = int(plot.height() * scale) + 1;
                QBitArray occupied(columns * rows);
                const auto marker_samples = display_sample_indices(
                    from, to, std::max<size_t>(2, static_cast<size_t>(std::ceil(plot.width() * scale * 2))));
                for (const size_t k : marker_samples) {
                    QPointF point(x(samples[k].time), plot.bottom() - (sample_value(k, channels_[ch]) - low) /
                                                                          (high - low) * plot.height());
                    if (!plot.contains(point))
                        continue;
                    int cell = int((point.y() - plot.top()) * scale) * columns +
                               int((point.x() - plot.left()) * scale);
                    if (occupied.testBit(cell))
                        continue;
                    occupied.setBit(cell);
                    paint_marker(painter, point, style, colors[int(ch) % colors.size()]);
                }
            }
            if (style.line == CurveLine::none)
                continue;
            if (stride > 4 && style.line == CurveLine::solid) {
                // Dense traces use a min/max envelope. Independent raster lines avoid
                // expensive antialiased joins through thousands of full-height edges.
                QList<QLineF> envelope;
                QPointF previous;
                bool have_previous = false;
                auto y = [&](double v) { return plot.bottom() - (v - low) / (high - low) * plot.height(); };
                for (size_t i = from; i < to; i += stride) {
                    const size_t finish = std::min(i + stride, to);
                    const double first_value = sample_value(i, channels_[ch]);
                    size_t imin=i, imax=i;
                    if (live_ || preview_channels_.contains(channels_[ch])) {
                        const size_t last_sample=finish-1;
                        if (sample_value(last_sample,channels_[ch])<first_value)imin=last_sample;
                        if (sample_value(last_sample,channels_[ch])>first_value)imax=last_sample;
                    } else {
                        const auto extrema=sample_extrema(channels_[ch],i,finish);
                        imin=extrema.first;imax=extrema.second;
                    }
                    const double minimum = sample_value(imin, channels_[ch]), maximum = sample_value(imax, channels_[ch]),
                                 last_value = sample_value(finish - 1, channels_[ch]);
                    const double column = x((samples[i].time + samples[finish - 1].time) / 2);
                    envelope.append(QLineF(column, y(minimum), column, y(maximum)));
                    if (have_previous) {
                        const QPointF first_point(column, y(first_value));
                        if (digital) {
                            envelope.append(QLineF(previous, QPointF(column, previous.y())));
                            envelope.append(QLineF(QPointF(column, previous.y()), first_point));
                        } else
                            envelope.append(QLineF(previous, first_point));
                    }
                    previous = QPointF(column, y(last_value));
                    have_previous = true;
                }
                painter.save();
                painter.setRenderHint(QPainter::Antialiasing, false);
                painter.setPen(QPen(colors[static_cast<int>(ch) % colors.size()], style.width));
                painter.drawLines(envelope);
                painter.restore();
                continue;
            }
            for (size_t i = from; i < to; i += stride) {
                size_t finish = std::min(i + stride, to);
                auto [imin, imax] = sample_extrema(channels_[ch], i, finish);
                std::vector<size_t> indices{i, imin, imax, finish - 1};
                std::sort(indices.begin(), indices.end());
                indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
                for (size_t k : indices) {
                    double t = samples[k].time, v = sample_value(k, channels_[ch]);
                    QPointF point(x(t), plot.bottom() - (v - low) / (high - low) * plot.height());
                    if (!started) {
                        path.moveTo(point);
                        started = true;
                    } else {
                        if (digital)
                            path.lineTo(point.x(), path.currentPosition().y());
                        path.lineTo(point);
                    }
                }
            }
            painter.setPen(
                QPen(colors[static_cast<int>(ch) % colors.size()], style.width, curve_pen(style.line)));
            painter.drawPath(path);
        }
        for (int index = 0; index < 2; ++index) {
            const double cursor = index == 0 ? cursor_a : cursor_b;
            (void)cursor;
            if (channel_display(cursor_channel(index)) != lane)
                continue;
            auto reading = cursor_reading(index);
            if (!reading || !channel_visible(reading->key) || reading->time < begin || reading->time > end)
                continue;
            const double cx = x(reading->time);
            painter.setPen(QPen(theme_colors().curves[index], 1, Qt::DashLine));
            painter.drawLine(QPointF(cx, plot.top()), QPointF(cx, plot.bottom()));
            const double cy = plot.bottom() - (reading->value - low) / (high - low) * plot.height();
            painter.setBrush(theme_colors().surface);
            painter.drawEllipse(QPointF(cx, cy), 4, 4);
            const QRectF tag(std::clamp(cx + 3, plot.left(), plot.right() - 16),
                             plot.bottom() - 20 - index * 17, 16, 16);
            auto tag_background = theme_colors().surface;
            tag_background.setAlpha(225);
            painter.fillRect(tag, tag_background);
            painter.drawText(tag, Qt::AlignCenter, index == 0 ? "A" : "B");
            if (screen_cursors_)
                painter.drawLine(QPointF(plot.left(), cy), QPointF(plot.right(), cy));
        }
        const bool trigger_visible=trigger_armed_||trigger_time_.has_value()||trigger_capture_until_.has_value();
        int trigger_channel=-1;
        for(int candidate:channels_)
            if(result_channel(*result_,candidate).object==trigger_channel_){trigger_channel=candidate;break;}
        const int trigger_lane=trigger_channel>=0?channel_display(trigger_channel):active_lane_;
        if(trigger_visible&&lane==trigger_lane) {
            const double raw_y=plot.bottom()-(trigger_level_-low)/(high-low)*plot.height();
            const double ty=std::clamp(raw_y,plot.top(),plot.bottom());
            painter.setPen(QPen(theme_colors().error,1.2,Qt::DashLine));
            painter.drawLine(QPointF(plot.left(),ty),QPointF(plot.right(),ty));
            painter.setBrush(theme_colors().error);
            painter.drawPolygon(QPolygonF{{plot.right(),ty},{plot.right()-8,ty-5},{plot.right()-8,ty+5}});
            painter.drawText(QPointF(plot.right()-52,std::max(plot.top()+13,ty-5)),
                             "T " + QString::number(trigger_level_,'g',5));
        }
        if(trigger_visible) {
            const double position_x=plot.left()+trigger_position_*plot.width();
            painter.setPen(QPen(theme_colors().error,1,Qt::DotLine));
            painter.drawLine(QPointF(position_x,plot.top()),QPointF(position_x,plot.bottom()));
            painter.setBrush(theme_colors().error);
            painter.drawPolygon(QPolygonF{{position_x,plot.top()+8},{position_x-5,plot.top()},{position_x+5,plot.top()}});
        }
        if (trigger_time_) {
            painter.setPen(QPen(theme_colors().error, 1, Qt::DashDotLine));
            double tx = x(*trigger_time_);
            painter.drawLine(QPointF(tx, plot.top()), QPointF(tx, plot.bottom()));
            painter.drawText(QPointF(tx + 4, plot.top() + 30), "T");
        }
        if (selecting_zoom_ && (!separate_axes_ || active_lane_ == lane)) {
            QRectF region(drag_origin_, drag_current_);
            region = region.normalized();
            if (zoom_axes == Axes::x) {
                region.setTop(plot.top());
                region.setBottom(plot.bottom());
            }
            if (zoom_axes == Axes::y) {
                region.setLeft(plot.left());
                region.setRight(plot.right());
            }
            painter.setPen(QPen(theme_colors().accent, 1, Qt::DashLine));
            painter.setBrush(QColor(57, 133, 202, 35));
            painter.drawRect(region);
        }
        painter.restore();
        paint_legend(painter, lane);
    }
}
} // namespace pds::desktop
