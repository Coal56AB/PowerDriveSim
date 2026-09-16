#pragma once
#include "core/model/model.hpp"
#include <QPainterPath>
#include <QRectF>
#include <optional>
#include <vector>
namespace pds::desktop {
QPainterPath orthogonal_route(QPointF from, QPointF from_stub, QPointF to_stub, QPointF to,
                              const std::vector<QRectF> &obstacles);
QPainterPath manual_route(QPointF from, QPointF to, const std::vector<Point> &bends);
std::vector<Point> route_bends(const QPainterPath &path);
std::vector<Point> clean_route_bends(QPointF from, QPointF to, const std::vector<Point> &bends);
QPointF project_on_route(const QPainterPath &path, QPointF point, int *segment = nullptr);
// Both paths terminate at the same explicit electrical endpoint. Reuse the
// trunk from the join to that endpoint; this never joins unrelated networks.
std::optional<std::pair<QPainterPath, QPointF>> join_route_to_trunk(const QPainterPath &branch, QPointF stub,
                                                                    const QPainterPath &trunk,
                                                                    const std::vector<QRectF> &obstacles);
} // namespace pds::desktop
