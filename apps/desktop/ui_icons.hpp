#pragma once
#include "apps/desktop/theme.hpp"
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>

namespace pds::desktop {
enum class UiIcon {
    zoom_x,
    zoom_y,
    zoom_xy,
    fit_x,
    fit_y,
    fit_xy,
    cursors,
    follow,
    time_window,
    measurements,
    settings,
    gear,
    export_data,
    clear,
    play,
    pause,
    stop,
    undock,
    dock,
    visible,
    hidden
};

// Paint in logical coordinates so toolbar symbols stay sharp at any display scale.
class UiIconEngine final : public QIconEngine {
  public:
    explicit UiIconEngine(UiIcon kind, bool light = false) : kind_(kind), light_(light) {}
    QIconEngine *clone() const override { return new UiIconEngine(*this); }
    void paint(QPainter *p, const QRect &rect, QIcon::Mode mode, QIcon::State) override {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const double side = std::min(rect.width(), rect.height());
        p->translate(rect.center().x() - side / 2, rect.center().y() - side / 2);
        p->scale(side / 24, side / 24);
        const QColor color(mode == QIcon::Disabled ? theme_colors().muted
                           : light_                ? QColor(Qt::white)
                                                   : theme_colors().text);
        p->setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->setBrush(Qt::NoBrush);
        auto line = [&](int x, int y, int a, int b) { p->drawLine(x, y, a, b); };
        auto horizontal = [&] {
            line(5, 12, 19, 12);
            line(5, 12, 8, 9);
            line(5, 12, 8, 15);
            line(19, 12, 16, 9);
            line(19, 12, 16, 15);
        };
        auto vertical = [&] {
            line(12, 5, 12, 19);
            line(12, 5, 9, 8);
            line(12, 5, 15, 8);
            line(12, 19, 9, 16);
            line(12, 19, 15, 16);
        };
        switch (kind_) {
        case UiIcon::zoom_x:
        case UiIcon::zoom_y:
        case UiIcon::zoom_xy: {
            p->drawEllipse(QRectF(2, 2, 15, 15));
            line(16, 16, 22, 22);
            auto font = p->font();
            font.setPixelSize(9);
            font.setBold(true);
            p->setFont(font);
            p->drawText(QRectF(2, 2, 15, 15), Qt::AlignCenter,
                        kind_ == UiIcon::zoom_x   ? "X"
                        : kind_ == UiIcon::zoom_y ? "Y"
                                                  : "XY");
            break;
        }
        case UiIcon::fit_x:
        case UiIcon::fit_y:
        case UiIcon::fit_xy:
            if (kind_ != UiIcon::fit_y) {
                horizontal();
                line(2, 6, 2, 18);
                line(22, 6, 22, 18);
            }
            if (kind_ != UiIcon::fit_x) {
                vertical();
                line(6, 2, 18, 2);
                line(6, 22, 18, 22);
            }
            break;
        case UiIcon::cursors:
            line(7, 3, 7, 21);
            line(17, 3, 17, 21);
            line(3, 17, 21, 7);
            p->setBrush(color);
            p->drawEllipse(QPointF(7, 15), 2, 2);
            p->drawEllipse(QPointF(17, 9), 2, 2);
            break;
        case UiIcon::follow:
            line(3, 17, 7, 17);
            line(7, 17, 10, 7);
            line(10, 7, 13, 17);
            line(13, 17, 20, 17);
            line(22, 3, 22, 21);
            line(14, 6, 19, 6);
            line(19, 6, 16, 3);
            line(19, 6, 16, 9);
            break;
        case UiIcon::time_window:
            p->drawEllipse(QRectF(3, 3, 18, 18));
            line(12, 6, 12, 12);
            line(12, 12, 17, 12);
            break;
        case UiIcon::measurements:
            p->drawRoundedRect(QRectF(3, 5, 18, 14), 1, 1);
            for (int x = 6; x < 21; x += 3)
                line(x, 5, x, x % 2 ? 9 : 12);
            break;
        case UiIcon::gear:
            for (int i = 0; i < 8; ++i) {
                p->save();
                p->translate(12, 12);
                p->rotate(i * 45);
                p->drawLine(QPointF(0, -7), QPointF(0, -10));
                p->restore();
            }
            p->drawEllipse(QRectF(5, 5, 14, 14));
            p->drawEllipse(QRectF(9, 9, 6, 6));
            break;
        case UiIcon::settings:
            for (int y = 5; y <= 19; y += 7) {
                line(3, y, 21, y);
                p->setBrush(theme_colors().surface);
                p->drawEllipse(QPointF(y == 12 ? 16 : 8, y), 2.5, 2.5);
            }
            break;
        case UiIcon::export_data:
            line(4, 14, 4, 21);
            line(4, 21, 20, 21);
            line(20, 21, 20, 14);
            line(12, 2, 12, 16);
            line(12, 16, 7, 11);
            line(12, 16, 17, 11);
            break;
        case UiIcon::clear:
            line(6, 6, 18, 18);
            line(6, 18, 18, 6);
            break;
        case UiIcon::play:
            p->setBrush(color);
            p->drawPolygon(QPolygonF{{7, 4}, {20, 12}, {7, 20}});
            break;
        case UiIcon::pause:
            p->setBrush(color);
            p->drawRect(QRectF(6, 4, 4, 16));
            p->drawRect(QRectF(15, 4, 4, 16));
            break;
        case UiIcon::stop:
            p->setBrush(color);
            p->drawRoundedRect(QRectF(5, 5, 14, 14), 1, 1);
            break;
        case UiIcon::undock:
        case UiIcon::dock:
            line(3, 7, 3, 21);
            line(3, 21, 17, 21);
            line(17, 21, 17, 17);
            line(3, 7, 7, 7);
            p->drawRect(QRectF(10, 3, 11, 11));
            if (kind_ == UiIcon::undock) {
                line(7, 17, 16, 8);
                line(16, 8, 12, 8);
                line(16, 8, 16, 12);
            } else {
                line(16, 8, 7, 17);
                line(7, 17, 7, 13);
                line(7, 17, 11, 17);
            }
            break;
        case UiIcon::visible:
        case UiIcon::hidden: {
            QPainterPath eye(QPointF(2, 12));
            eye.cubicTo(8, 3, 16, 3, 22, 12);
            eye.cubicTo(16, 21, 8, 21, 2, 12);
            p->drawPath(eye);
            p->drawEllipse(QPointF(12, 12), 3, 3);
            if (kind_ == UiIcon::hidden)
                line(3, 3, 21, 21);
            break;
        }
        }
        p->restore();
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap image(size);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return image;
    }

  private:
    UiIcon kind_;
    bool light_;
};
inline QIcon ui_icon(UiIcon kind, bool light = false) {
    return QIcon(new UiIconEngine(kind, light));
}
} // namespace pds::desktop
