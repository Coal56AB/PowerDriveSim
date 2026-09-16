#pragma once
#include "apps/desktop/theme.hpp"
#include "core/model/model.hpp"
#include <QPainter>
#include <QPainterPath>
namespace pds::desktop {
inline Qt::PenStyle curve_pen(CurveLine line) {
    switch (line) {
    case CurveLine::dash:
        return Qt::DashLine;
    case CurveLine::dot:
        return Qt::DotLine;
    case CurveLine::dash_dot:
        return Qt::DashDotLine;
    case CurveLine::none:
        return Qt::NoPen;
    default:
        return Qt::SolidLine;
    }
}
inline void paint_marker(QPainter &p, QPointF center, const CurveStyle &style, QColor color) {
    if (style.marker == CurveMarker::none)
        return;
    const double r = style.marker_size / 2;
    p.save();
    p.translate(center);
    p.setPen(QPen(color, std::min(style.width, 2.)));
    p.setBrush(theme_colors().surface);
    switch (style.marker) {
    case CurveMarker::circle:
        p.drawEllipse(QPointF(), r, r);
        break;
    case CurveMarker::square:
        p.drawRect(QRectF(-r, -r, 2 * r, 2 * r));
        break;
    case CurveMarker::triangle:
    case CurveMarker::triangle_down: {
        const double d = style.marker == CurveMarker::triangle ? 1 : -1;
        p.drawPolygon(QPolygonF{{0, -r * d}, {r, r * d}, {-r, r * d}});
        break;
    }
    case CurveMarker::diamond:
        p.drawPolygon(QPolygonF{{0, -r}, {r, 0}, {0, r}, {-r, 0}});
        break;
    case CurveMarker::cross:
        p.drawLine(QPointF(-r, -r), QPointF(r, r));
        p.drawLine(QPointF(-r, r), QPointF(r, -r));
        break;
    case CurveMarker::plus:
        p.drawLine(QPointF(-r, 0), QPointF(r, 0));
        p.drawLine(QPointF(0, -r), QPointF(0, r));
        break;
    default:
        break;
    }
    p.restore();
}
} // namespace pds::desktop
