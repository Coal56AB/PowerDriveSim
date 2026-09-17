#include "apps/desktop/scope_style.hpp"
#include "apps/desktop/editor.hpp"
#include "apps/desktop/number_input.hpp"
#include "apps/desktop/theme.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <algorithm>
#include <cmath>
namespace pds::desktop {
QString Scope::curve_name(const std::string &key) const {
    for (const auto &[channel, name] : curve_names_)
        if (channel == key)
            return QString::fromStdString(name);
    if (result_)
        for (int channel : channels_) {
            auto info = result_channel(*result_, channel);
            if (info.object == key)
                return QString::fromStdString(info.name);
        }
    return QString::fromStdString(key);
}
void Scope::set_curve_name(const std::string &key, const QString &name) {
    if (key.empty())
        return;
    const auto trimmed = name.trimmed();
    std::erase_if(curve_names_, [&](const auto &entry) { return entry.first == key; });
    if (!trimmed.isEmpty())
        curve_names_.emplace_back(key, trimmed.toStdString());
    if (pending_options_)
        pending_options_->curve_names = curve_names_;
    update_channel_controls();
    if (changed)
        changed(begin, end, cursor_a, cursor_b);
    update();
}
CurveStyle Scope::curve_style(const std::string &key) const {
    for (const auto &style : curve_styles_)
        if (style.channel == key)
            return style;
    return {key, CurveLine::solid, line_width_, CurveMarker::none, 6};
}
double Scope::curve_multiplier(const std::string &key) const {
    for (const auto &[channel, multiplier] : curve_multipliers_)
        if (channel == key)
            return multiplier;
    return 1.0;
}
void Scope::set_curve_multiplier(const std::string &key, double value) {
    if (key.empty() || !std::isfinite(value))
        return;
    std::erase_if(curve_multipliers_, [&](const auto &entry) { return entry.first == key; });
    if (std::abs(value - 1.0) > 1e-15)
        curve_multipliers_.emplace_back(key, value);
    if (pending_options_)
        pending_options_->curve_multipliers = curve_multipliers_;
    extrema_.clear();
    fit_y();
    update_channel_controls();
    if (changed)
        changed(begin, end, cursor_a, cursor_b);
    update();
}
void Scope::show_multiplier_settings(const std::string &key) {
    if (key.empty())
        return;
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName("curve_multiplier_settings");
    dialog->setWindowTitle(text("curve_multiplier_title").arg(curve_name(key)));
    auto *form = new QFormLayout(dialog);
    auto *multiplier = new QLineEdit(QString::number(curve_multiplier(key), 'g', 12));
    multiplier->setObjectName("curve_multiplier");
    normalize_decimal_point(multiplier);
    form->addRow(text("curve_multiplier"), multiplier);
    auto *error = new QLabel;
    error->setStyleSheet("color:palette(bright-text)");
    form->addRow(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, dialog,
            [this, key, multiplier, error] {
                bool ok = false;
                const double value = multiplier->text().toDouble(&ok);
                if (!ok || !std::isfinite(value)) {
                    error->setText(text("curve_multiplier_error"));
                    return;
                }
                set_curve_multiplier(key, value);
                error->clear();
            });
    dialog->show();
    multiplier->selectAll();
    multiplier->setFocus();
}
void Scope::set_curve_style(const CurveStyle &style) {
    if (style.channel.empty() || unsigned(style.line) > unsigned(CurveLine::none) ||
        unsigned(style.marker) > unsigned(CurveMarker::triangle_down) || !std::isfinite(style.width) ||
        style.width <= 0 || style.width > 10 || !std::isfinite(style.marker_size) || style.marker_size < 1 ||
        style.marker_size > 24)
        return;
    auto it = std::find_if(curve_styles_.begin(), curve_styles_.end(),
                           [&](const auto &s) { return s.channel == style.channel; });
    if (it == curve_styles_.end())
        curve_styles_.push_back(style);
    else
        *it = style;
    if (pending_options_)
        pending_options_->curve_styles = curve_styles_;
    if (changed)
        changed(begin, end, cursor_a, cursor_b);
    update();
}
void Scope::show_curve_settings(const std::string &key) {
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName("curve_settings");
    QString name = QString::fromStdString(key);
    if (result_)
        for (int channel : channels_) {
            auto info = result_channel(*result_, channel);
            if (info.object == key)
                name = (curve_name(info.object) + QString::fromStdString(" [" + info.unit + "]"));
        }
    dialog->setWindowTitle(text("curve_style").arg(name));
    dialog->setProperty("channel", QString::fromStdString(key));
    auto *form = new QFormLayout(dialog);
    const auto style = curve_style(key);
    auto *line = new QComboBox;
    line->setObjectName("curve_line");
    for (auto label : {"line_solid", "line_dash", "line_dot", "line_dash_dot", "line_none"})
        line->addItem(text(label));
    line->setCurrentIndex(int(style.line));
    form->addRow(text("line_type"), line);
    auto *width = new QLineEdit(QString::number(style.width));
    width->setObjectName("curve_width");
    normalize_decimal_point(width);
    form->addRow(text("line_width"), width);
    auto *marker = new QComboBox;
    marker->setObjectName("curve_marker");
    for (auto label : {"marker_none", "marker_circle", "marker_square", "marker_triangle", "marker_diamond",
                       "marker_cross", "marker_plus", "marker_down"})
        marker->addItem(text(label));
    marker->setCurrentIndex(int(style.marker));
    form->addRow(text("marker_shape"), marker);
    auto *size = new QLineEdit(QString::number(style.marker_size));
    size->setObjectName("curve_marker_size");
    normalize_decimal_point(size);
    form->addRow(text("marker_size"), size);
    auto *multiplier = new QLineEdit(QString::number(curve_multiplier(key), 'g', 12));
    multiplier->setObjectName("curve_multiplier");
    normalize_decimal_point(multiplier);
    form->addRow(text("curve_multiplier"), multiplier);
    auto *error = new QLabel;
    error->setStyleSheet("color:palette(bright-text)");
    form->addRow(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, dialog, [=, this] {
        bool width_ok, size_ok, multiplier_ok;
        double w = width->text().toDouble(&width_ok), s = size->text().toDouble(&size_ok);
        double m = multiplier->text().toDouble(&multiplier_ok);
        if (!width_ok || !size_ok || !multiplier_ok || !std::isfinite(w) || !std::isfinite(s) ||
            !std::isfinite(m) || w <= 0 || w > 10 || s < 1 || s > 24) {
            error->setText(text("curve_style_error"));
            return;
        }
        set_curve_style({key, CurveLine(line->currentIndex()), w, CurveMarker(marker->currentIndex()), s});
        set_curve_multiplier(key, m);
        error->clear();
    });
    dialog->show();
}
Scope::LegendLayout Scope::legend_layout(int lane) const {
    LegendLayout layout;
    if (!result_ || !(show_legend_ || separate_axes_))
        return layout;
    const auto plot = lane_rect(lane).adjusted(4, 4, -4, -4);
    double width = 0;
    const double row_height = fontMetrics().height() + 6;
    for (int i = 0; i < int(channels_.size()); ++i) {
        const auto info = result_channel(*result_, channels_[i]);
        if (channel_display(channels_[i]) != lane || !channel_visible(info.object))
            continue;
        width =
            std::max(width, double(fontMetrics().horizontalAdvance(
                                (curve_name(info.object) + QString::fromStdString(" [" + info.unit + "]")))) +
                                56);
        layout.rows.emplace_back(i, QRectF());
    }
    if (layout.rows.empty())
        return layout;
    double height = row_height * layout.rows.size() + 10;
    width = std::min(width, plot.width());
    height = std::min(height, plot.height());
    double x = 0, y = 0;
    for (const auto &pos : legend_positions_)
        if (pos.display == unsigned(lane)) {
            x = pos.x;
            y = pos.y;
        }
    layout.box = {plot.left() + x * std::max(0., plot.width() - width),
                  plot.top() + y * std::max(0., plot.height() - height), width, height};
    for (size_t i = 0; i < layout.rows.size(); ++i)
        layout.rows[i].second = {layout.box.left() + 7, layout.box.top() + 5 + i * row_height, width - 14,
                                 row_height};
    return layout;
}
void Scope::paint_legend(QPainter &painter, int lane) {
    const auto layout = legend_layout(lane);
    if (layout.box.isEmpty())
        return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(theme_colors().border, 1));
    auto background = theme_colors().surface;
    background.setAlpha(225);
    painter.setBrush(background);
    painter.drawRoundedRect(layout.box, 4, 4);
    painter.setClipRect(layout.box.adjusted(2, 2, -2, -2));
    const auto &colors = theme_colors().curves;
    for (const auto &[i, row] : layout.rows) {
        auto info = result_channel(*result_, channels_[i]);
        auto style = curve_style(info.object);
        const auto color = colors[i % 5];
        painter.setPen(QPen(color, style.width, curve_pen(style.line)));
        painter.drawLine(QPointF(row.left(), row.center().y()), QPointF(row.left() + 26, row.center().y()));
        paint_marker(painter, {row.left() + 13, row.center().y()}, style, color);
        painter.setPen(theme_colors().text);
        painter.drawText(row.adjusted(34, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
                         (curve_name(info.object) + QString::fromStdString(" [" + info.unit + "]")));
    }
    painter.restore();
}
int Scope::legend_at(QPointF point) const {
    for (int lane = 0; lane < display_count(); ++lane)
        if (legend_layout(lane).box.contains(point))
            return lane;
    return -1;
}
void Scope::move_legend(QPointF point) {
    if (!legend_drag_moved_ && (point - drag_origin_).manhattanLength() < QApplication::startDragDistance())
        return;
    legend_drag_moved_ = true;
    const auto area = lane_rect(legend_drag_lane_).adjusted(4, 4, -4, -4);
    auto top = legend_drag_box_.topLeft() + point - drag_origin_;
    LegendPosition pos{
        unsigned(legend_drag_lane_),
        std::clamp((top.x() - area.left()) / std::max(1., area.width() - legend_drag_box_.width()), 0., 1.),
        std::clamp((top.y() - area.top()) / std::max(1., area.height() - legend_drag_box_.height()), 0., 1.)};
    std::erase_if(legend_positions_, [&](const auto &p) { return p.display == pos.display; });
    legend_positions_.push_back(pos);
    update();
}
} // namespace pds::desktop
