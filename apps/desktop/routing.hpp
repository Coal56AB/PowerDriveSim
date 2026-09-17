#pragma once
#include "core/model/model.hpp"
#include <QPainterPath>
#include <QRectF>
#include <vector>
namespace pds::desktop {
QPainterPath orthogonal_route(QPointF from, QPointF from_stub, QPointF to_stub, QPointF to,
                              const std::vector<QRectF> &obstacles);
QPainterPath manual_route(QPointF from, QPointF to, const std::vector<Point> &bends);
std::vector<Point> route_bends(const QPainterPath &path);
std::vector<Point> clean_route_bends(QPointF from, QPointF to, const std::vector<Point> &bends);
QPointF project_on_route(const QPainterPath &path, QPointF point, int *segment = nullptr);
} // namespace pds::desktop
