#include "apps/desktop/routing.hpp"
#include <QLineF>
#include <algorithm>
#include <cmath>
#include <limits>
namespace pds::desktop {
static std::vector<QPointF> clean_points(const std::vector<QPointF> &points) {
    std::vector<QPointF> clean;
    for (auto point : points) {
        if (!clean.empty() && point == clean.back())
            continue;
        while (clean.size() > 1) {
            auto u = clean.back() - clean[clean.size() - 2], v = point - clean.back();
            // A moved endpoint can pass an old bend. Collapse collinear
            // backtracking too, otherwise the obsolete bend leaves a spur.
            if (std::abs(u.x() * v.y() - u.y() * v.x()) > 1e-8)
                break;
            clean.pop_back();
        }
        if (clean.empty() || point != clean.back())
            clean.push_back(point);
    }
    return clean;
}
static QPainterPath clean_route(const std::vector<QPointF> &points) {
    const auto clean = clean_points(points);
    QPainterPath path;
    if (!clean.empty()) {
        path.moveTo(clean.front());
        for (size_t i = 1; i < clean.size(); ++i)
            path.lineTo(clean[i]);
    }
    return path;
}
QPainterPath manual_route(QPointF a, QPointF b, const std::vector<Point> &bends) {
    std::vector<QPointF> points{a};
    for (auto point : bends) {
        points.push_back({point.x, points.back().y()});
        points.push_back({point.x, point.y});
    }
    points.push_back({b.x(), points.back().y()});
    points.push_back(b);
    return clean_route(points);
}
std::vector<Point> route_bends(const QPainterPath &path) {
    std::vector<QPointF> points;
    points.reserve(size_t(path.elementCount()));
    for (int i = 0; i < path.elementCount(); ++i) {
        auto e = path.elementAt(i);
        points.push_back({e.x, e.y});
    }
    points = clean_points(points);
    std::vector<Point> result;
    for (size_t i = 1; i + 1 < points.size(); ++i)
        result.push_back({points[i].x(), points[i].y()});
    return result;
}
std::vector<Point> clean_route_bends(QPointF from, QPointF to, const std::vector<Point> &bends) {
    return route_bends(manual_route(from, to, bends));
}
QPointF project_on_route(const QPainterPath &path, QPointF point, int *segment) {
    double best = std::numeric_limits<double>::infinity();
    QPointF joint = point;
    for (int i = 1; i < path.elementCount(); ++i) {
        auto ea = path.elementAt(i - 1), eb = path.elementAt(i);
        QPointF a(ea.x, ea.y), b(eb.x, eb.y), d = b - a;
        double length = QPointF::dotProduct(d, d);
        double t = length ? std::clamp(QPointF::dotProduct(point - a, d) / length, 0., 1.) : 0;
        auto candidate = a + t * d;
        double distance = QPointF::dotProduct(point - candidate, point - candidate);
        if (distance < best) {
            best = distance;
            joint = candidate;
            if (segment)
                *segment = i;
        }
    }
    return joint;
}
QPainterPath orthogonal_route(QPointF a, QPointF sa, QPointF sb, QPointF b, const std::vector<QRectF> &all) {
    // Local obstacles bound the search independently of the total scene size.
    QRectF area = QRectF(a, b).normalized().adjusted(-120, -120, 120, 120);
    std::vector<QRectF> boxes;
    for (auto box : all)
        if (box.intersects(area))
            boxes.push_back(box);
    auto clear = [&](const std::vector<QPointF> &route) {
        for (size_t i = 1; i < route.size(); ++i)
            for (const auto &box : boxes) {
                auto x = route[i - 1], y = route[i];
                if (x.x() == y.x() && x.x() > box.left() && x.x() < box.right() &&
                    std::max(x.y(), y.y()) > box.top() && std::min(x.y(), y.y()) < box.bottom())
                    return false;
                if (x.y() == y.y() && x.y() > box.top() && x.y() < box.bottom() &&
                    std::max(x.x(), y.x()) > box.left() && std::min(x.x(), y.x()) < box.right())
                    return false;
            }
        return true;
    };
    // If the endpoints already share an axis, a straight segment is the
    // canonical route. In particular, a branch ending at a junction must form
    // an exact T instead of following a port stub and overlapping the trunk.
    if ((std::abs(a.x() - b.x()) < 1e-8 || std::abs(a.y() - b.y()) < 1e-8) && clear({a, b}))
        return clean_route({a, b});
    double score = std::numeric_limits<double>::infinity();
    std::vector<QPointF> best;
    auto consider = [&](std::vector<QPointF> route) {
        if (!clear(route))
            return;
        if (QPointF::dotProduct(route[1] - route[0], route[2] - route[1]) < 0 ||
            QPointF::dotProduct(route.back() - route[route.size() - 2],
                                route[route.size() - 2] - route[route.size() - 3]) < 0)
            return;
        double length = 0;
        for (size_t i = 1; i < route.size(); ++i)
            length += QLineF(route[i - 1], route[i]).length();
        if (length < score) {
            score = length;
            best = std::move(route);
        }
    };
    consider({a, sa, {sa.x(), sb.y()}, sb, b});
    consider({a, sa, {sb.x(), sa.y()}, sb, b});
    consider({a, sa, {(sa.x() + sb.x()) / 2, sa.y()}, {(sa.x() + sb.x()) / 2, sb.y()}, sb, b});
    consider({a, sa, {sa.x(), (sa.y() + sb.y()) / 2}, {sb.x(), (sa.y() + sb.y()) / 2}, sb, b});
    for (auto box : boxes) {
        for (double x : {box.left() - 20, box.right() + 20})
            consider({a, sa, {x, sa.y()}, {x, sb.y()}, sb, b});
        for (double y : {box.top() - 20, box.bottom() + 20})
            consider({a, sa, {sa.x(), y}, {sb.x(), y}, sb, b});
    }
    if (best.empty())
        best = {a, sa, {sa.x(), sb.y()}, sb, b};
    // Remove zero length and collinear vertices: each visible handle has a purpose.
    return clean_route(best);
}
} // namespace pds::desktop
