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
#include <cmath>
namespace pds::desktop {
static QPointF snap(QPointF p) {
    return {std::round(p.x() / 20) * 20, std::round(p.y() / 20) * 20};
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
    return gesture_ == Gesture::moving || gesture_ == Gesture::placing || gesture_ == Gesture::wiring ||
           gesture_ == Gesture::routing || gesture_ == Gesture::reconnecting;
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
QPointF Canvas::insertion_position() const {
    return mapToScene(viewport()->rect().contains(last_mouse_) ? last_mouse_ : viewport()->rect().center());
}
void Canvas::set_ghost(QGraphicsItem *item) {
    const auto position = ghost_ ? ghost_->pos() : snap(insertion_position());
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
        auto joint = project_on_route(wire->path(), mapToScene(p));
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
    wire_origin_ = {source_.point.x, source_.point.y};
    wire_preview_ = scene()->addPath(QPainterPath(wire_origin_), QPen(QColor("#467fe0"), 1.6, Qt::DashLine));
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
    if (gesture_ == Gesture::moving) {
        for (auto [item, pos] : positions_) {
            item->setPos(pos);
            item->setTransform(transforms_.at(item));
        }
        move_transform_.reset();
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
    guides_.clear();
    route_item_ = nullptr;
    cancel_wire();
    gesture_ = Gesture::idle;
    unsetCursor();
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
        QGraphicsView::mousePressEvent(e);
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
            place(e->modifiers() & Qt::AltModifier ? mapToScene(e->pos()) : snap(mapToScene(e->pos())));
        e->accept();
        return;
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
    if (editable_ && (port_at(e->pos()) || (wire && (e->modifiers() & Qt::ControlModifier)))) {
        begin_wire(anchor_at(e->pos(), false));
        scroll_timer_.start();
        e->accept();
        return;
    }
    auto *object = object_at(e->pos());
    QGraphicsView::mousePressEvent(e);
    if (!object && wire) {
        if (!(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)))
            scene()->clearSelection();
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
        ghost_->setPos(free ? pos : snap(pos));
        ghost_->show();
    }
    if (gesture_ == Gesture::moving && dragged_ && !positions_.empty()) {
        QPointF delta = pos - press_scene_;
        auto anchor = move_base_.map(positions_.at(move_anchor_));
        if (!free)
            delta = snap(anchor + delta) - anchor;
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
    } else if ((gesture_ == Gesture::wiring || gesture_ == Gesture::reconnecting) && wire_preview_) {
        auto target = anchor_at(point, true);
        auto end = QPointF(target.point.x, target.point.y);
        if (target.endpoint.object.empty() && target.wire.empty())
            end = free ? pos : snap(pos);
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
        wire_preview_->setPen(
            QPen(QColor(!valid ? "#cb5563" : (test ? "#16a085" : "#467fe0")), 1.6, Qt::DashLine));
        viewport()->update();
    } else if (gesture_ == Gesture::routing && route_item_ && dragged_) {
        auto points = original_route_;
        auto delta = pos - press_scene_;
        if (!free)
            delta = snap(delta);
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
    if (port_at(e->pos()))
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
    if (gesture_ == Gesture::panning)
        gesture_ = resume_;
    if (gesture_ == Gesture::wiring || gesture_ == Gesture::reconnecting) {
        move_gesture(e->pos(), e->modifiers());
        auto from = source_, to = anchor_at(e->pos(), true);
        auto replace = edited_wire_;
        auto bends = wire_preview_ ? route_bends(wire_preview_->path()) : std::vector<Point>{};
        bool commit = dragged_ && viewport()->rect().contains(e->pos());
        if (to.endpoint.object.empty() && to.wire.empty()) {
            auto p = e->modifiers() & Qt::AltModifier ? mapToScene(e->pos()) : snap(mapToScene(e->pos()));
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
    bool moved = gesture_ == Gesture::moving && dragged_;
    gesture_ = Gesture::idle;
    guides_.clear();
    QGraphicsView::mouseReleaseEvent(e);
    if (moved && released)
        released();
    positions_.clear();
    transforms_.clear();
    move_anchor_ = nullptr;
    move_transform_.reset();
    viewport()->update();
}
void Canvas::mouseDoubleClickEvent(QMouseEvent *e) {
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
    p->fillRect(rect, QColor("#f4f7fb"));
    if (transform().m11() < .3)
        return;
    p->setPen(QPen(QColor("#d9e2ed"), 0));
    for (double x = std::floor(rect.left() / 20) * 20; x < rect.right(); x += 20)
        for (double y = std::floor(rect.top() / 20) * 20; y < rect.bottom(); y += 20)
            p->drawPoint(QPointF(x, y));
}
void Canvas::drawForeground(QPainter *p, const QRectF &) {
    p->save();
    p->setPen(QPen(QColor("#b579bc"), 0, Qt::DashLine));
    for (const auto &line : guides_)
        p->drawLine(line);
    p->restore();
    p->save();
    p->resetTransform();
    p->setPen(QPen(QColor("#467fe0"), 1));
    p->setBrush(Qt::white);
    for (auto *item : scene()->selectedItems())
        if (item->data(1).toString() == "wire") {
            auto path = static_cast<QGraphicsPathItem *>(item)->path();
            const int segment = item->data(wire_segment_role).toInt();
            for (int i = 0; i < path.elementCount(); ++i) {
                if (segment > 0 && i != segment - 1 && i != segment)
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
        p->setPen(QColor("#45607f"));
        p->drawText(viewport()->rect().adjusted(30, -65, -30, -65), Qt::AlignCenter, text("empty_title"));
        font.setPointSize(11);
        font.setBold(false);
        p->setFont(font);
        p->drawText(viewport()->rect().adjusted(30, 30, -30, 30), Qt::AlignCenter, text("empty_steps"));
    }
    p->restore();
}
} // namespace pds::desktop
