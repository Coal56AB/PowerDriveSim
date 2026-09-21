#include "apps/desktop/theme.hpp"
#include "apps/desktop/editor.hpp"
#include "apps/desktop/routing.hpp"
#include <QApplication>
#include <QContextMenuEvent>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>
namespace pds::desktop {
static QPointF snap_to(QPointF p, double grid) {
    // A consistent half-up rule is translation invariant across zero. std::round
    // rounds negative halves away from zero, which made ports jump by one grid
    // cell after an object crossed the origin and the scene was rebuilt.
    const auto snap = [grid](double value) { return std::floor(value / grid + 0.5) * grid; };
    return {snap(p.x()), snap(p.y())};
}
Canvas::Canvas(QWidget *parent) : QGraphicsView(parent) {
    setScene(new QGraphicsScene(this));
    setRenderHint(QPainter::Antialiasing);
    setDragMode(RubberBandDrag);
    setTransformationAnchor(NoAnchor);
    setViewportUpdateMode(BoundingRectViewportUpdate);
    setObjectName("canvas");
    setMouseTracking(true);
    scroll_timer_.setInterval(16);
    connect(&scroll_timer_, &QTimer::timeout, this, [this] {
        if (!editing_gesture() || gesture_ == Gesture::placing || gesture_ == Gesture::panning)
            return;
        auto edge = [](int p, int size) {
            return p < 28 ? -std::min(16, 28 - p) : (p > size - 28 ? std::min(16, p - size + 28) : 0);
        };
        int x = edge(last_mouse_.x(), viewport()->width()), y = edge(last_mouse_.y(), viewport()->height());
        if (x || y) {
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() + x);
            verticalScrollBar()->setValue(verticalScrollBar()->value() + y);
            move_gesture(last_mouse_, QApplication::keyboardModifiers());
        }
    });
}
bool Canvas::editing_gesture() const {
    if (gesture_ == Gesture::panning)
        return resume_ != Gesture::idle && resume_ != Gesture::selecting;
    return gesture_ == Gesture::moving || gesture_ == Gesture::scaling || gesture_ == Gesture::moving_port ||
           gesture_ == Gesture::placing || gesture_ == Gesture::wiring || gesture_ == Gesture::routing ||
           gesture_ == Gesture::reconnecting;
}
void Canvas::set_grid_size(double size) {
    grid_size_ = std::clamp(size, 5.0, 100.0);
    viewport()->update();
}
void Canvas::set_grid_style(GridStyle style) {
    grid_style_ = style;
    viewport()->update();
}
void Canvas::set_grid_line_width(double width) {
    grid_line_width_ = std::clamp(width, 0.1, 8.0);
    viewport()->update();
}
void Canvas::set_grid_dot_size(double size) {
    grid_dot_size_ = std::clamp(size, 0.5, 8.0);
    viewport()->update();
}
QPointF Canvas::snap_point(QPointF point) const {
    return snap_to(point, grid_size_);
}
bool Canvas::transform_move(int turns, bool mirror) {
    if (gesture_ != Gesture::moving || !move_anchor_)
        return false;
    const auto center = move_anchor_->pos();
    QTransform change;
    change.translate(center.x(), center.y());
    if (mirror)
        change.scale(-1, 1);
    else
        change.rotate(90 * turns);
    change.translate(-center.x(), -center.y());
    move_base_ = move_transform_ * change;
    press_scene_ = mapToScene(last_mouse_);
    dragged_ = true;
    move_gesture(last_mouse_, Qt::AltModifier);
    return true;
}
Point Canvas::moving_point(const std::string &from, const std::string &to, Point point) const {
    bool a = false, b = false;
    for (const auto &[item, pos] : positions_) {
        if (item->data(1).toString() == "label")
            continue;
        const auto id = item->data(0).toString().toStdString();
        a |= id == from;
        b |= id == to;
    }
    if (a && b) {
        const auto p = move_transform_.map(QPointF(point.x, point.y));
        return {p.x(), p.y()};
    }
    return point;
}
void Canvas::set_editable(bool enabled) {
    if (!enabled)
        cancel_gesture();
    editable_ = enabled;
}
void Canvas::start_connect_mode() {
    if (!editable_)
        return;
    cancel_gesture();
    connect_mode_ = true;
    setCursor(Qt::CrossCursor);
}
QPointF Canvas::insertion_position() const {
    return mapToScene(viewport()->rect().contains(last_mouse_) ? last_mouse_ : viewport()->rect().center());
}
void Canvas::set_ghost(QGraphicsItem *item) {
    const auto position = ghost_ ? ghost_->pos() : snap_point(insertion_position());
    if (ghost_) {
        scene()->removeItem(ghost_);
        delete ghost_;
    }
    ghost_ = item;
    if (item) {
        item->setOpacity(.42);
        item->setFlags({});
        item->setZValue(90);
        std::function<void(QGraphicsItem *)> disable = [&](QGraphicsItem *child) {
            child->setAcceptedMouseButtons(Qt::NoButton);
            child->setAcceptHoverEvents(false);
            child->setData(1, "preview");
            child->setFlags({});
            for (auto *c : child->childItems())
                disable(c);
        };
        disable(item);
        scene()->addItem(item);
        item->setPos(position);
        gesture_ = Gesture::placing;
    } else if (gesture_ == Gesture::placing)
        gesture_ = Gesture::idle;
}
void Canvas::leaveEvent(QEvent *e) {
    if (ghost_)
        ghost_->hide();
    QGraphicsView::leaveEvent(e);
}
QGraphicsItem *Canvas::object_at(QPoint p) const {
    for (auto *item : items(p)) {
        auto *root = item;
        while (root->parentItem())
            root = root->parentItem();
        if (root != ghost_ && root != wire_preview_ && !root->data(0).toString().isEmpty())
            return root;
    }
    return nullptr;
}
QGraphicsItem *Canvas::public_pin_at(QPoint point) const {
    const auto scene_point = mapToScene(point);
    for (auto *root : scene()->selectedItems()) {
        if (!root || root->data(1).toString() != "atom" ||
            (!root->data(10).toBool() && !root->data(12).toBool()))
            continue;
        const QRectF body = root->shape().boundingRect();
        const bool plot = root->data(12).toBool();
        for (auto *child : root->childItems()) {
            if (child->data(1).toString() != "port")
                continue;
            const QPointF local = child->pos();
            const double left_x = plot ? -60.0 : body.left() - 10.0;
            const double right_x = plot ? 60.0 : body.right() + 10.0;
            const double top_y = plot ? body.top() - 20.0 : body.top() - 10.0;
            const double bottom_y = plot ? body.bottom() + 20.0 : body.bottom() + 10.0;
            const double vertical_edge_distance = std::min(std::abs(local.x() - left_x), std::abs(local.x() - right_x));
            const double horizontal_edge_distance = std::min(std::abs(local.y() - top_y), std::abs(local.y() - bottom_y));
            QPointF local_edge;
            if (vertical_edge_distance <= horizontal_edge_distance)
                local_edge = {local.x() < body.center().x() ? body.left() : body.right(),
                              std::clamp(local.y(), body.top(), body.bottom())};
            else
                local_edge = {std::clamp(local.x(), body.left(), body.right()),
                              local.y() < body.center().y() ? body.top() : body.bottom()};
            const QPointF port = child->scenePos();
            const QPointF edge = root->mapToScene(local_edge);
            QLineF lead(edge, port);
            if (lead.length() < 1e-6)
                continue;
            const QPointF v = port - edge;
            const QPointF w = scene_point - edge;
            const double t = std::clamp(QPointF::dotProduct(v, w) / QPointF::dotProduct(v, v), 0.0, 1.0);
            const QPointF closest = edge + v * t;
            // The outer end remains a normal connection target. Drag the inner
            // part of the short lead to relocate the public pin.
            if (t <= .7 && QLineF(scene_point, closest).length() <= 8.0 / transform().m11())
                return child;
        }
    }
    return nullptr;
}
struct ResizeHit {
    QPointF origin;
    bool x = true, y = true;
};
static std::optional<ResizeHit> resize_corner(QGraphicsItem *item, QPoint view_pos, const QGraphicsView *view) {
    if (!item || item->data(1).toString() == "wire" || item->data(1).toString() == "label" ||
        !item->isSelected())
        return {};
    const auto r = item->boundingRect();
    const std::array<std::pair<QPointF, QPointF>, 4> corners{
        std::pair{r.topLeft(), r.bottomRight()}, std::pair{r.topRight(), r.bottomLeft()},
        std::pair{r.bottomLeft(), r.topRight()}, std::pair{r.bottomRight(), r.topLeft()}};
    for (auto [handle, origin] : corners) {
        const auto screen = view->mapFromScene(item->mapToScene(handle));
        if (QLineF(QPointF(view_pos), QPointF(screen)).length() <= 14)
            return ResizeHit{item->mapToScene(origin), true, true};
    }
    const std::array<std::tuple<QPointF, QPointF, bool, bool>, 4> sides{
        std::tuple{QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()), true, false},
        std::tuple{QPointF(r.right(), r.center().y()), QPointF(r.left(), r.center().y()), true, false},
        std::tuple{QPointF(r.center().x(), r.top()), QPointF(r.center().x(), r.bottom()), false, true},
        std::tuple{QPointF(r.center().x(), r.bottom()), QPointF(r.center().x(), r.top()), false, true}};
    for (auto [handle, origin, x, y] : sides) {
        const auto screen = view->mapFromScene(item->mapToScene(handle));
        if (QLineF(QPointF(view_pos), QPointF(screen)).length() <= 14)
            return ResizeHit{item->mapToScene(origin), x, y};
    }
    return {};
}
std::optional<Endpoint> Canvas::port_at(QPoint p) const {
    std::optional<Endpoint> best;
    double distance = 121;
    for (auto *item : items(QRect(p - QPoint(11, 11), QSize(22, 22))))
        if (item->data(1).toString() == "port") {
            auto delta = mapFromScene(item->scenePos()) - p;
            double d = QPoint::dotProduct(delta, delta);
            Endpoint endpoint{item->data(0).toString().toStdString(), item->data(2).toString().toStdString()};
            if (d < distance) {
                distance = d;
                best = endpoint;
            }
        }
    return best;
}
std::optional<Endpoint> Canvas::hovered_port() const {
    return port_at(last_mouse_);
}
QGraphicsPathItem *Canvas::wire_at(QPoint p) const {
    QGraphicsPathItem *best = nullptr;
    double distance = 9;
    for (auto *item : items(QRect(p - QPoint(9, 9), QSize(18, 18))))
        if (item->data(1).toString() == "wire") {
            auto *wire = static_cast<QGraphicsPathItem *>(item);
            auto joint = project_on_route(wire->path(), mapToScene(p));
            double d = QLineF(QPointF(p), QPointF(mapFromScene(joint))).length();
            if (d < distance) {
                distance = d;
                best = wire;
            }
        }
    return best;
}
WireAnchor Canvas::anchor_at(QPoint p, bool prefer_valid) const {
    WireAnchor result;
    result.point = {mapToScene(p).x(), mapToScene(p).y()};
    // Prefer compatible ports within a fixed screen radius, then show the closest invalid target.
    QGraphicsItem *best = nullptr;
    double distance = 121;
    for (auto *item : items(QRect(p - QPoint(11, 11), QSize(22, 22))))
        if (item->data(1).toString() == "port") {
            Endpoint endpoint{item->data(0).toString().toStdString(), item->data(2).toString().toStdString()};
            auto delta = mapFromScene(item->scenePos()) - p;
            double d = QPoint::dotProduct(delta, delta);
            if (prefer_valid && (!source_.endpoint.object.empty()) && compatible &&
                !compatible(source_.endpoint, endpoint))
                continue;
            if (d < distance) {
                distance = d;
                best = item;
            }
        }
    if (best) {
        result.endpoint = {best->data(0).toString().toStdString(), best->data(2).toString().toStdString()};
        result.point = {best->scenePos().x(), best->scenePos().y()};
        return result;
    }
    if (auto *wire = wire_at(p); wire && wire->data(0).toString().toStdString() != edited_wire_) {
        result.wire = wire->data(0).toString().toStdString();
        int segment = 0;
        auto joint = project_on_route(wire->path(), mapToScene(p), &segment);
        if ((gesture_ == Gesture::wiring || gesture_ == Gesture::reconnecting) && segment > 0 &&
            segment < wire->path().elementCount()) {
            const auto a = wire->path().elementAt(segment - 1), b = wire->path().elementAt(segment);
            const QPointF start(source_.point.x, source_.point.y);
            const auto between = [](double v, double x, double y) {
                return v >= std::min(x, y) - 1e-6 && v <= std::max(x, y) + 1e-6;
            };
            if (std::abs(a.y - b.y) < 1e-6 && between(start.x(), a.x, b.x))
                joint = {start.x(), a.y};
            else if (std::abs(a.x - b.x) < 1e-6 && between(start.y(), a.y, b.y))
                joint = {a.x, start.y()};
        }
        result.point = {joint.x(), joint.y()};
        for (int i = 0; i < wire->path().elementCount(); ++i) {
            auto e = wire->path().elementAt(i);
            result.route.push_back({e.x, e.y});
        }
    } else if (prefer_valid)
        return anchor_at(p, false);
    return result;
}
void Canvas::begin_wire(WireAnchor source) {
    source_ = std::move(source);
    connect_mode_ = false;
    wire_origin_ = {source_.point.x, source_.point.y};
    wire_preview_color_ = QColor("#467fe0");
    if (!source_.endpoint.object.empty())
        for (auto *item : scene()->items())
            if (item->data(1).toString() == "port" &&
                item->data(0).toString().toStdString() == source_.endpoint.object &&
                item->data(2).toString().toStdString() == source_.endpoint.port) {
                const QColor color(item->data(11).toString());
                if (color.isValid())
                    wire_preview_color_ = color;
                break;
            }
    wire_preview_ = scene()->addPath(QPainterPath(wire_origin_), QPen(wire_preview_color_, 1.6, Qt::DashLine));
    wire_preview_->setZValue(100);
    wire_preview_->setAcceptedMouseButtons(Qt::NoButton);
    gesture_ = Gesture::wiring;
}
void Canvas::cancel_wire() {
    if (wire_preview_) {
        scene()->removeItem(wire_preview_);
        delete wire_preview_;
        wire_preview_ = nullptr;
    }
    source_ = {};
    edited_wire_.clear();
    if (gesture_ == Gesture::wiring || gesture_ == Gesture::reconnecting)
        gesture_ = Gesture::idle;
    viewport()->update();
}
void Canvas::cancel_gesture() {
    scroll_timer_.stop();
    if (gesture_ == Gesture::panning)
        gesture_ = resume_;
    if (gesture_ == Gesture::moving || gesture_ == Gesture::scaling) {
        for (auto [item, pos] : positions_) {
            item->setPos(pos);
            item->setTransform(transforms_.at(item));
        }
        move_transform_.reset();
        if (movement)
            movement();
    }
    if (gesture_ == Gesture::moving_port && port_anchor_) {
        port_anchor_->setPos(port_start_);
        if (movement)
            movement();
    }
    if (gesture_ == Gesture::routing && route_item_) {
        QPainterPath p(original_route_.front());
        for (auto point : original_route_)
            p.lineTo(point);
        route_item_->setPath(p);
    }
    positions_.clear();
    transforms_.clear();
    move_anchor_ = nullptr;
    port_anchor_ = nullptr;
    guides_.clear();
    route_item_ = nullptr;
    cancel_wire();
    gesture_ = Gesture::idle;
    unsetCursor();
    connect_mode_ = false;
    set_ghost(nullptr);
    if (cancel_placement)
        cancel_placement();
    viewport()->update();
}
void Canvas::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape && editing_gesture()) {
        cancel_gesture();
        e->accept();
        return;
    }
    QGraphicsView::keyPressEvent(e);
}
bool Canvas::viewportEvent(QEvent *e) {
    if ((e->type() == QEvent::WindowDeactivate || e->type() == QEvent::UngrabMouse) && editing_gesture() &&
        gesture_ != Gesture::placing)
        cancel_gesture();
    return QGraphicsView::viewportEvent(e);
}
void Canvas::mousePressEvent(QMouseEvent *e) {
    last_mouse_ = e->pos();
    setFocus();
    if (e->button() == Qt::MiddleButton) {
        resume_ = gesture_;
        gesture_ = Gesture::panning;
        pan_origin_ = e->pos();
        setCursor(Qt::ClosedHandCursor);
        e->accept();
        return;
    }
    if (e->button() != Qt::LeftButton) {
        // A context click must not collapse an existing multi-selection. The
        // context handler decides whether an unselected target becomes the
        // new selection after it has captured the selected group.
        e->accept();
        return;
    }
    if (editing_gesture() && gesture_ != Gesture::placing) {
        e->accept();
        return;
    }
    press_ = e->pos();
    press_scene_ = mapToScene(press_);
    dragged_ = false;
    if (ghost_ && editable_) {
        if (place)
            place(e->modifiers() & Qt::AltModifier ? mapToScene(e->pos()) : snap_point(mapToScene(e->pos())));
        e->accept();
        return;
    }
    auto *object = object_at(e->pos());
    if (editable_ && connect_mode_ && !object && !port_at(e->pos()) && !wire_at(e->pos())) {
        const auto point = snap_point(mapToScene(e->pos()));
        connect_mode_ = false;
        unsetCursor();
        if (add_junction)
            add_junction(point);
        e->accept();
        return;
    }
    if (editable_) {
        if (auto *pin = public_pin_at(e->pos())) {
            positions_.clear();
            transforms_.clear();
            port_anchor_ = pin;
            port_start_ = pin->pos();
            press_scene_ = mapToScene(e->pos());
            gesture_ = Gesture::moving_port;
            scroll_timer_.start();
            e->accept();
            return;
        }
        QGraphicsItem *resize_target = object;
        std::optional<ResizeHit> resize_origin;
        if (resize_target && !port_at(e->pos()))
            resize_origin = resize_corner(resize_target, e->pos(), this);
        if (!resize_origin && !port_at(e->pos()))
            for (auto *item : scene()->selectedItems())
                if ((resize_origin = resize_corner(item, e->pos(), this))) {
                    resize_target = item;
                    break;
                }
        if (resize_origin && resize_target) {
            positions_.clear();
            transforms_.clear();
            move_anchor_ = resize_target;
            scale_origin_ = resize_origin->origin;
            scale_x_axis_ = resize_origin->x;
            scale_y_axis_ = resize_origin->y;
            scale_start_distance_ = std::max(1.0, QLineF(scale_origin_, press_scene_).length());
            for (auto *item : scene()->selectedItems())
                if (item->data(1).toString() != "wire" &&
                    item->data(1).toString() == resize_target->data(1).toString()) {
                    positions_[item] = item->pos();
                    transforms_[item] = item->transform();
                }
            gesture_ = Gesture::scaling;
            scroll_timer_.start();
            e->accept();
            return;
        }
    }
    if (editable_ && object && !port_at(e->pos())) {
        if (auto origin = resize_corner(object, e->pos(), this)) {
            positions_.clear();
            transforms_.clear();
            move_anchor_ = object;
            scale_origin_ = origin->origin;
            scale_x_axis_ = origin->x;
            scale_y_axis_ = origin->y;
            scale_start_distance_ = std::max(1.0, QLineF(scale_origin_, press_scene_).length());
            for (auto *item : scene()->selectedItems())
                if (item->data(1).toString() != "wire" && item->data(1).toString() == object->data(1).toString()) {
                    positions_[item] = item->pos();
                    transforms_[item] = item->transform();
                }
            gesture_ = Gesture::scaling;
            scroll_timer_.start();
            e->accept();
            return;
        }
    }
    auto *wire = wire_at(e->pos());
    if (editable_ && wire && wire->isSelected() && !(e->modifiers() & Qt::ControlModifier)) {
        const auto path = wire->path();
        auto pos = mapToScene(e->pos());
        // Endpoint handles on a selected wire take precedence over a port underneath.
        bool first = QLineF(QPointF(e->pos()), QPointF(mapFromScene(path.pointAtPercent(0)))).length() < 9;
        bool last = QLineF(QPointF(e->pos()), QPointF(mapFromScene(path.pointAtPercent(1)))).length() < 9;
        if ((first || last) && wire_endpoint) {
            WireAnchor source;
            source.endpoint = wire_endpoint(wire->data(0).toString().toStdString(), !first);
            auto fixed = first ? path.pointAtPercent(1) : path.pointAtPercent(0);
            source.point = {fixed.x(), fixed.y()};
            begin_wire(source);
            edited_wire_ = wire->data(0).toString().toStdString();
            gesture_ = Gesture::reconnecting;
            e->accept();
            scroll_timer_.start();
            return;
        }
        if (!port_at(e->pos())) {
            route_item_ = wire;
            original_route_.clear();
            for (int i = 0; i < path.elementCount(); ++i) {
                auto a = path.elementAt(i);
                original_route_.push_back({a.x, a.y});
            }
            project_on_route(path, pos, &segment_);
            for (auto *selected : scene()->selectedItems())
                if (selected != wire)
                    selected->setSelected(false);
            wire->setData(wire_segment_role, segment_);
            if (movement)
                movement();
            viewport()->update();
            vertex_ = -1;
            for (int i = 1; i + 1 < static_cast<int>(original_route_.size()); ++i)
                if (QLineF(QPointF(e->pos()), QPointF(mapFromScene(original_route_[i]))).length() < 8) {
                    vertex_ = i;
                    break;
                }
            gesture_ = Gesture::routing;
            scroll_timer_.start();
            e->accept();
            return;
        }
    }
    if (editable_ && (port_at(e->pos()) || (wire && ((e->modifiers() & Qt::ControlModifier) || connect_mode_)))) {
        begin_wire(anchor_at(e->pos(), false));
        scroll_timer_.start();
        e->accept();
        return;
    }
    QGraphicsView::mousePressEvent(e);
    if (!object && wire) {
        if (!(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)))
            scene()->clearSelection();
        wire->setData(wire_segment_role, 0);
        wire->setSelected(true);
    }
    if (editable_ && object && object->data(1).toString() != "wire" && object->isSelected()) {
        positions_.clear();
        transforms_.clear();
        move_anchor_ = object;
        move_transform_.reset();
        move_base_.reset();
        for (auto *item : scene()->selectedItems())
            if (item->data(1).toString() != "wire" &&
                item->data(1).toString() == object->data(1).toString()) {
                positions_[item] = item->pos();
                transforms_[item] = item->transform();
            }
        gesture_ = Gesture::moving;
        scroll_timer_.start();
    } else
        gesture_ = Gesture::selecting;
}
void Canvas::move_gesture(QPoint point, Qt::KeyboardModifiers modifiers) {
    auto pos = mapToScene(point);
    bool free = modifiers & Qt::AltModifier;
    if ((point - press_).manhattanLength() >= QApplication::startDragDistance())
        dragged_ = true;
    if (ghost_) {
        ghost_->setPos(free ? pos : snap_point(pos));
        ghost_->show();
    }
    if (gesture_ == Gesture::moving && dragged_ && !positions_.empty()) {
        QPointF delta = pos - press_scene_;
        auto anchor = move_base_.map(positions_.at(move_anchor_));
        if (!free)
            delta = snap_point(anchor + delta) - anchor;
        guides_.clear();
        if (!free) {
            auto target = anchor + delta;
            double dx = 7 / transform().m11(), dy = dx;
            std::optional<double> gx, gy;
            for (auto *item : scene()->items())
                if (!item->parentItem() && !positions_.count(item) && item->data(1).toString() != "wire" &&
                    !item->data(0).toString().isEmpty() && item->data(1) == move_anchor_->data(1)) {
                    if (std::abs(item->pos().x() - target.x()) < dx) {
                        dx = std::abs(item->pos().x() - target.x());
                        gx = item->pos().x();
                    }
                    if (std::abs(item->pos().y() - target.y()) < dy) {
                        dy = std::abs(item->pos().y() - target.y());
                        gy = item->pos().y();
                    }
                }
            auto bounds = mapToScene(viewport()->rect()).boundingRect();
            if (gx) {
                delta.setX(*gx - anchor.x());
                guides_.emplace_back(*gx, bounds.top(), *gx, bounds.bottom());
            }
            if (gy) {
                delta.setY(*gy - anchor.y());
                guides_.emplace_back(bounds.left(), *gy, bounds.right(), *gy);
            }
        }
        QTransform translation;
        translation.translate(delta.x(), delta.y());
        move_transform_ = move_base_ * translation;
        QTransform linear(move_transform_.m11(), move_transform_.m12(), move_transform_.m21(),
                          move_transform_.m22(), 0, 0);
        for (auto [item, start] : positions_) {
            item->setPos(move_transform_.map(start));
            item->setTransform(transforms_.at(item) * linear);
        }
        if (movement)
            movement();
        viewport()->update();
    } else if (gesture_ == Gesture::scaling && dragged_ && !positions_.empty()) {
        const auto start = press_scene_ - scale_origin_;
        const auto current = pos - scale_origin_;
        double raw_factor = std::clamp(std::max(1.0, QLineF(scale_origin_, pos).length()) / scale_start_distance_, 0.25, 4.0);
        double sx = raw_factor, sy = raw_factor;
        if (!scale_x_axis_ && scale_y_axis_)
            sx = 1.0;
        if (scale_x_axis_ && !scale_y_axis_)
            sy = 1.0;
        if (scale_x_axis_ && !scale_y_axis_ && std::abs(start.x()) > 1.0)
            sx = std::clamp(std::abs(current.x() / start.x()), 0.25, 4.0);
        if (!scale_x_axis_ && scale_y_axis_ && std::abs(start.y()) > 1.0)
            sy = std::clamp(std::abs(current.y() / start.y()), 0.25, 4.0);
        for (auto [item, original_pos] : positions_) {
            double item_sx = sx, item_sy = sy;
            const auto base = transforms_.at(item);
            const bool framed = item->data(10).toBool() || item->data(12).toBool();
            if (framed) {
                const QRectF local_frame = item->data(14).toRectF();
                const QPointF origin = base.map(QPointF());
                const double base_x = QLineF(origin, base.map(QPointF(1, 0))).length();
                const double base_y = QLineF(origin, base.map(QPointF(0, 1))).length();
                const double width = std::max(grid_size_, std::round(local_frame.width() * base_x * item_sx / grid_size_) * grid_size_);
                const double height = std::max(grid_size_, std::round(local_frame.height() * base_y * item_sy / grid_size_) * grid_size_);
                item_sx = width / (local_frame.width() * base_x);
                item_sy = height / (local_frame.height() * base_y);
            } else {
                const double step = std::max(0.05, grid_size_ / 180.0);
                item_sx = std::clamp(std::round(item_sx / step) * step, 0.25, 4.0);
                item_sy = std::clamp(std::round(item_sy / step) * step, 0.25, 4.0);
            }
            QTransform scale;
            scale.scale(item_sx, item_sy);
            item->setPos(original_pos);
            item->setTransform(base * scale);
            if (framed) {
                const QRectF frame = item->mapRectToScene(item->data(14).toRectF());
                const auto half_grid = [this](double value) {
                    return std::round((value - grid_size_ / 2.0) / grid_size_) * grid_size_ + grid_size_ / 2.0;
                };
                item->setPos(item->pos() + QPointF(half_grid(frame.left()) - frame.left(),
                                                    half_grid(frame.top()) - frame.top()));
            }
        }
        if (movement)
            movement();
        viewport()->update();
    } else if (gesture_ == Gesture::moving_port && port_anchor_) {
        auto *parent = port_anchor_->parentItem();
        if (!parent)
            return;
        QPointF local = parent->mapFromScene(pos);
        if (!free)
            local = parent->mapFromScene(snap_point(parent->mapToScene(local)));
        const QRectF body = parent->shape().boundingRect();
        const bool plot = parent->data(12).toBool();
        const std::array<QPointF, 4> candidates = {
            QPointF(plot ? -60.0 : body.left() - 10.0, std::clamp(local.y(), body.top(), body.bottom())),
            QPointF(plot ? 60.0 : body.right() + 10.0, std::clamp(local.y(), body.top(), body.bottom())),
            QPointF(std::clamp(local.x(), body.left(), body.right()), plot ? body.top() - 20.0 : body.top() - 10.0),
            QPointF(std::clamp(local.x(), body.left(), body.right()), plot ? body.bottom() + 20.0 : body.bottom() + 10.0)};
        const auto closest = std::min_element(candidates.begin(), candidates.end(), [&](QPointF a, QPointF b) {
            return QLineF(local, a).length() < QLineF(local, b).length();
        });
        QPointF target = *closest;
        if (parent->data(10).toBool() || plot) {
            const std::array<QPointF, 4> edges = {
                QPointF(body.left(), std::clamp(local.y(), body.top(), body.bottom())),
                QPointF(body.right(), std::clamp(local.y(), body.top(), body.bottom())),
                QPointF(std::clamp(local.x(), body.left(), body.right()), body.top()),
                QPointF(std::clamp(local.x(), body.left(), body.right()), body.bottom())};
            const auto edge = *std::min_element(edges.begin(), edges.end(), [&](QPointF a, QPointF b) {
                return QLineF(local, a).length() < QLineF(local, b).length();
            });
            QPointF outward;
            if (edge.x() == body.left()) outward = {-1, 0};
            else if (edge.x() == body.right()) outward = {1, 0};
            else if (edge.y() == body.top()) outward = {0, -1};
            else outward = {0, 1};
            const QPointF edge_scene = parent->mapToScene(edge);
            QPointF direction = parent->mapToScene(edge + outward) - edge_scene;
            const double length = std::hypot(direction.x(), direction.y());
            if (length > 1e-9)
                target = parent->mapFromScene(snap_point(edge_scene + direction / length * (grid_size_ / 2.0)));
        }
        const bool vertical_side = target.x() < body.left() || target.x() > body.right();
        const QPointF tangent = vertical_side ? QPointF(0, 1) : QPointF(1, 0);
        const double tangent_scale = QLineF(parent->mapToScene(QPointF()), parent->mapToScene(tangent)).length();
        const double step = std::max(1e-6, grid_size_ / std::max(1e-9, tangent_scale));
        const auto siblings = parent->childItems();
        auto occupied = [&](QPointF point) {
            return std::any_of(siblings.begin(), siblings.end(), [&](QGraphicsItem *sibling) {
                return sibling != port_anchor_ && sibling->data(1).toString() == "port" &&
                       QLineF(point, sibling->pos()).length() < .5;
            });
        };
        if (occupied(target)) {
            const double low = vertical_side ? body.top() : body.left();
            const double high = vertical_side ? body.bottom() : body.right();
            const double origin = vertical_side ? target.y() : target.x();
            std::optional<QPointF> free_slot;
            for (int distance = 1; distance <= 64 && !free_slot; ++distance)
                for (int direction : {1, -1}) {
                    const double coordinate = origin + direction * distance * step;
                    if (coordinate < low - 1e-6 || coordinate > high + 1e-6)
                        continue;
                    QPointF candidate = target;
                    if (vertical_side)
                        candidate.setY(coordinate);
                    else
                        candidate.setX(coordinate);
                    if (!occupied(candidate)) {
                        free_slot = candidate;
                        break;
                    }
                }
            if (!free_slot) {
                std::vector<QPointF> alternatives;
                const double first_y = std::ceil(body.top() / step) * step;
                for (double y = first_y; y <= body.bottom() + 1e-6; y += step) {
                    alternatives.push_back({candidates[0].x(), y});
                    alternatives.push_back({candidates[1].x(), y});
                }
                const double first_x = std::ceil(body.left() / step) * step;
                for (double x = first_x; x <= body.right() + 1e-6; x += step) {
                    alternatives.push_back({x, candidates[2].y()});
                    alternatives.push_back({x, candidates[3].y()});
                }
                std::erase_if(alternatives, occupied);
                if (!alternatives.empty())
                    free_slot = *std::min_element(alternatives.begin(), alternatives.end(),
                                                  [&](QPointF a, QPointF b) {
                                                      return QLineF(target, a).length() < QLineF(target, b).length();
                                                  });
            }
            if (!free_slot)
                return; // The original non-overlapping position is retained.
            target = *free_slot;
        }
        port_anchor_->setPos(target);
        port_anchor_->setData(9, true);
        if (movement)
            movement();
        viewport()->update();
    } else if ((gesture_ == Gesture::wiring || gesture_ == Gesture::reconnecting) && wire_preview_) {
        auto target = anchor_at(point, true);
        auto end = QPointF(target.point.x, target.point.y);
        if (target.endpoint.object.empty() && target.wire.empty())
            end = free ? pos : snap_point(pos);
        auto a = source_.endpoint;
        if (a.object.empty() && !source_.wire.empty() && wire_endpoint)
            a = wire_endpoint(source_.wire, true);
        std::optional<Endpoint> b;
        if (!target.endpoint.object.empty())
            b = target.endpoint;
        auto test = b;
        if (!test && !target.wire.empty() && wire_endpoint)
            test = wire_endpoint(target.wire, true);
        bool valid = !test || (!compatible || compatible(a, *test));
        auto path =
            route_preview ? route_preview(a, b, wire_origin_, end) : manual_route(wire_origin_, end, {});
        wire_preview_->setPath(path);
        wire_preview_->setPen(QPen(valid ? wire_preview_color_ : QColor("#cb5563"), 1.6, Qt::DashLine));
        viewport()->update();
    } else if (gesture_ == Gesture::routing && route_item_ && dragged_) {
        auto points = original_route_;
        auto delta = pos - press_scene_;
        if (!free)
            delta = snap_to(delta, grid_size_);
        if (vertex_ >= 1) {
            points[vertex_] += delta;
        } else if (segment_ > 0 && segment_ < static_cast<int>(points.size())) {
            bool horizontal = points[segment_ - 1].y() == points[segment_].y();
            QPointF shift = horizontal ? QPointF(0, delta.y()) : QPointF(delta.x(), 0);
            auto left = points[segment_ - 1] + shift, right = points[segment_] + shift;
            if (segment_ == static_cast<int>(points.size()) - 1)
                points.insert(points.end() - 1, right);
            else
                points[segment_] = right;
            if (segment_ == 1)
                points.insert(points.begin() + 1, left);
            else
                points[segment_ - 1] = left;
        }
        std::vector<Point> bends;
        for (size_t i = 1; i + 1 < points.size(); ++i)
            bends.push_back({points[i].x(), points[i].y()});
        route_item_->setPath(manual_route(points.front(), points.back(), bends));
        int selected = 0;
        project_on_route(route_item_->path(), pos, &selected);
        route_item_->setData(wire_segment_role, selected);
        viewport()->update();
    }
}
void Canvas::mouseMoveEvent(QMouseEvent *e) {
    viewport()->update(QRect(last_mouse_ - QPoint(14, 14), QSize(28, 28)));
    viewport()->update(QRect(e->pos() - QPoint(14, 14), QSize(28, 28)));
    last_mouse_ = e->pos();
    if (gesture_ == Gesture::panning) {
        auto delta = e->pos() - pan_origin_;
        pan_origin_ = e->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        auto pan = gesture_;
        gesture_ = resume_;
        move_gesture(e->pos(), e->modifiers());
        gesture_ = pan;
        e->accept();
        return;
    }
    move_gesture(e->pos(), e->modifiers());
    if (editing_gesture()) {
        e->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(e);
    std::optional<ResizeHit> resize_hit;
    if (editable_) {
        if (auto *object = object_at(e->pos()))
            resize_hit = resize_corner(object, e->pos(), this);
        if (!resize_hit)
            for (auto *item : scene()->selectedItems())
                if ((resize_hit = resize_corner(item, e->pos(), this)))
                    break;
    }
    if (!editing_gesture() && editable_ && public_pin_at(e->pos()))
        setCursor(Qt::SizeVerCursor);
    else if (resize_hit)
        setCursor(resize_hit->x && !resize_hit->y   ? Qt::SizeHorCursor
                  : !resize_hit->x && resize_hit->y ? Qt::SizeVerCursor
                                                     : Qt::SizeFDiagCursor);
    else if (connect_mode_ || port_at(e->pos()))
        setCursor(Qt::CrossCursor);
    else
        unsetCursor();
}
void Canvas::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() == Qt::MiddleButton && gesture_ == Gesture::panning) {
        gesture_ = resume_;
        move_gesture(e->pos(), e->modifiers());
        unsetCursor();
        e->accept();
        return;
    }
    if (e->button() != Qt::LeftButton) {
        QGraphicsView::mouseReleaseEvent(e);
        return;
    }
    scroll_timer_.stop();
    if (gesture_ == Gesture::moving_port && port_anchor_ && !dragged_) {
        WireAnchor source;
        source.endpoint = {port_anchor_->data(0).toString().toStdString(),
                           port_anchor_->data(2).toString().toStdString()};
        const QPointF point = port_anchor_->scenePos();
        source.point = {point.x(), point.y()};
        port_anchor_ = nullptr;
        gesture_ = Gesture::idle;
        begin_wire(std::move(source));
        e->accept();
        return;
    }
    if (gesture_ == Gesture::panning)
        gesture_ = resume_;
    if (gesture_ == Gesture::wiring || gesture_ == Gesture::reconnecting) {
        move_gesture(e->pos(), e->modifiers());
        auto from = source_, to = anchor_at(e->pos(), true);
        auto replace = edited_wire_;
        auto bends = wire_preview_ ? route_bends(wire_preview_->path()) : std::vector<Point>{};
        bool commit = dragged_ && viewport()->rect().contains(e->pos());
        if (to.endpoint.object.empty() && to.wire.empty()) {
            auto p = e->modifiers() & Qt::AltModifier ? mapToScene(e->pos()) : snap_point(mapToScene(e->pos()));
            to.point = {p.x(), p.y()};
        }
        cancel_wire();
        if (commit && connect_wire)
            connect_wire(from, to, bends, replace);
        e->accept();
        return;
    }
    if (gesture_ == Gesture::routing) {
        move_gesture(e->pos(), e->modifiers());
        auto id = route_item_->data(0).toString().toStdString();
        auto bends = route_bends(route_item_->path());
        bool commit = dragged_;
        route_item_ = nullptr;
        gesture_ = Gesture::idle;
        if (commit && edit_route)
            edit_route(id, bends);
        e->accept();
        return;
    }
    if (gesture_ == Gesture::moving)
        move_gesture(e->pos(), e->modifiers());
    if (gesture_ == Gesture::moving_port)
        move_gesture(e->pos(), e->modifiers());
    bool moved = (gesture_ == Gesture::moving || gesture_ == Gesture::scaling ||
                  gesture_ == Gesture::moving_port) &&
                 dragged_;
    gesture_ = Gesture::idle;
    guides_.clear();
    QGraphicsView::mouseReleaseEvent(e);
    if (moved && released)
        released();
    positions_.clear();
    transforms_.clear();
    move_anchor_ = nullptr;
    port_anchor_ = nullptr;
    move_transform_.reset();
    viewport()->update();
}
void Canvas::mouseDoubleClickEvent(QMouseEvent *e) {
    // A composite always opens directly. Do not create an inline name editor
    // before hierarchy navigation; a stale editor can otherwise keep stealing
    // focus when entering or leaving a locked definition.
    if (auto *object = object_at(e->pos()); object && object->data(10).toBool()) {
        cancel_wire();
        gesture_ = Gesture::idle;
        if (open_object)
            open_object(object->data(0).toString().toStdString());
        e->accept();
        return;
    }
    if (editable_ && edit_text && edit_text(e->pos())) {
        e->accept();
        return;
    }
    if (auto *wire = wire_at(e->pos()); wire && wire->isSelected()) {
        mousePressEvent(e);
        return;
    }
    cancel_wire();
    gesture_ = Gesture::idle;
    auto *object = object_at(e->pos());
    if (object && open_object)
        open_object(object->data(0).toString().toStdString());
    else if (editable_ && quick_insert)
        quick_insert(mapToScene(e->pos()));
    e->accept();
}
void Canvas::contextMenuEvent(QContextMenuEvent *e) {
    if (editing_gesture()) {
        cancel_gesture();
        return;
    }
    auto *item = object_at(e->pos());
    if (context_menu)
        context_menu(item ? item->data(0).toString().toStdString() : std::string(), e->globalPos());
}
void Canvas::wheelEvent(QWheelEvent *e) {
    double delta = e->pixelDelta().isNull() ? e->angleDelta().y() / 120. : e->pixelDelta().y() / 80.;
    if (delta == 0) {
        e->accept();
        return;
    }
    auto before = mapToScene(e->position().toPoint());
    double old = transform().m11();
    double next = std::clamp(old * std::pow(1.15, delta), .15, 5.);
    scale(next / old, next / old);
    auto after = mapToScene(e->position().toPoint());
    translate(after.x() - before.x(), after.y() - before.y());
    last_mouse_ = e->position().toPoint();
    move_gesture(last_mouse_, e->modifiers());
    e->accept();
}
void Canvas::drawBackground(QPainter *p, const QRectF &rect) {
    p->fillRect(rect, theme_colors().canvas);
    if (grid_style_ == GridStyle::hidden || transform().m11() < .3)
        return;
    if (grid_style_ == GridStyle::lines) {
        p->setPen(QPen(theme_colors().grid, grid_line_width_));
        for (double x = std::floor(rect.left() / grid_size_) * grid_size_; x < rect.right(); x += grid_size_)
            p->drawLine(QLineF(x, rect.top(), x, rect.bottom()));
        for (double y = std::floor(rect.top() / grid_size_) * grid_size_; y < rect.bottom(); y += grid_size_)
            p->drawLine(QLineF(rect.left(), y, rect.right(), y));
    } else {
        p->setPen(Qt::NoPen);
        p->setBrush(theme_colors().grid);
        const double radius = grid_dot_size_ / 2.0;
        for (double x = std::floor(rect.left() / grid_size_) * grid_size_; x < rect.right(); x += grid_size_)
            for (double y = std::floor(rect.top() / grid_size_) * grid_size_; y < rect.bottom(); y += grid_size_)
                p->drawEllipse(QPointF(x, y), radius, radius);
    }
}
void Canvas::drawForeground(QPainter *p, const QRectF &) {
    p->save();
    p->setPen(QPen(QColor("#b579bc"), 0, Qt::DashLine));
    for (const auto &line : guides_)
        p->drawLine(line);
    p->restore();
    p->save();
    p->resetTransform();
    p->setPen(QPen(theme_colors().accent, 1));
    p->setBrush(theme_colors().surface);
    for (auto *item : scene()->selectedItems())
        if (item->isVisible() && item->data(1).toString() != "wire" &&
            item->data(1).toString() != "label" && item->parentItem() == nullptr) {
            const auto r = item->boundingRect();
            const std::array<QPointF, 4> corners{r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()};
            for (auto corner : corners) {
                const auto pos = mapFromScene(item->mapToScene(corner));
                p->drawRoundedRect(QRectF(pos.x() - 5, pos.y() - 5, 10, 10), 2, 2);
            }
            const std::array<QPointF, 2> x_handles{QPointF(r.left(), r.center().y()),
                                                   QPointF(r.right(), r.center().y())};
            for (auto handle : x_handles) {
                const auto pos = mapFromScene(item->mapToScene(handle));
                p->drawRoundedRect(QRectF(pos.x() - 4, pos.y() - 8, 8, 16), 3, 3);
                p->drawLine(QPointF(pos.x(), pos.y() - 4), QPointF(pos.x(), pos.y() + 4));
            }
            const std::array<QPointF, 2> y_handles{QPointF(r.center().x(), r.top()),
                                                   QPointF(r.center().x(), r.bottom())};
            for (auto handle : y_handles) {
                const auto pos = mapFromScene(item->mapToScene(handle));
                p->drawRoundedRect(QRectF(pos.x() - 8, pos.y() - 4, 16, 8), 3, 3);
                p->drawLine(QPointF(pos.x() - 4, pos.y()), QPointF(pos.x() + 4, pos.y()));
            }
        }
    for (auto *item : scene()->selectedItems())
        if (item->data(1).toString() == "wire") {
            auto path = static_cast<QGraphicsPathItem *>(item)->path();
            const int segment = item->data(wire_segment_role).toInt();
            if (segment <= 0)
                continue;
            for (int i = 0; i < path.elementCount(); ++i) {
                if (i != segment - 1 && i != segment)
                    continue;
                auto v = path.elementAt(i);
                auto pos = mapFromScene(QPointF(v.x, v.y));
                p->drawRect(QRectF(pos.x() - 3, pos.y() - 3, 6, 6));
            }
        }
    if (auto endpoint = hovered_port())
        for (auto *item : items(QRect(last_mouse_ - QPoint(11, 11), QSize(22, 22))))
            if (item->data(1).toString() == "port" &&
                item->data(0).toString().toStdString() == endpoint->object &&
                item->data(2).toString().toStdString() == endpoint->port) {
                p->setBrush(QColor(70, 127, 224, 35));
                p->drawEllipse(QPointF(mapFromScene(item->scenePos())), 7, 7);
                break;
            }
    if (scene()->items().empty()) {
        auto font = p->font();
        font.setPointSize(20);
        font.setBold(true);
        p->setFont(font);
        p->setPen(theme_colors().muted);
        p->drawText(viewport()->rect().adjusted(30, -65, -30, -65), Qt::AlignCenter, text("empty_title"));
        font.setPointSize(11);
        font.setBold(false);
        p->setFont(font);
        p->drawText(viewport()->rect().adjusted(30, 30, -30, 30), Qt::AlignCenter, text("empty_steps"));
    }
    p->restore();
}
} // namespace pds::desktop
