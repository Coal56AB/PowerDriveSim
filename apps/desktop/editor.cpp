#include "apps/desktop/editor.hpp"
#include "apps/desktop/instrumentation.hpp"
#include "apps/desktop/code_editor.hpp"
#include "apps/desktop/labels.hpp"
#include "apps/desktop/number_input.hpp"
#include "apps/desktop/routing.hpp"
#include "apps/desktop/signal_presets.hpp"
#include "apps/desktop/theme.hpp"
#include "apps/desktop/ui_icons.hpp"
#include "apps/desktop/code_icon_editor.hpp"
#include "core/editor/properties.hpp"
#include "core/model/hierarchy.hpp"
#include "core/model/c_program.hpp"
#include "formats/project/project.hpp"
#include "results/csv.hpp"
#include <QApplication>
#include <QGraphicsSceneHoverEvent>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QFormLayout>
#include <QGraphicsEllipseItem>
#include <QGraphicsItemGroup>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QDebug>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>
#include <algorithm>
#include <cmath>
#include <limits>
#include <iostream>
#include <sstream>
namespace pds::desktop {
static QString q(const std::string &s) {
    return QString::fromStdString(s);
}
static QColor domain_color(Domain domain) {
    return domain == Domain::gate ? QColor("#17866d")
         : domain == Domain::signal ? QColor("#8c67c8") : QColor("#146cca");
}
static QString tag_symbol(Domain domain) {
    return domain == Domain::gate ? QString("G")
         : domain == Domain::signal ? QString("S") : QString("N");
}
static void prune_orphan_nodes(Schematic &schematic) {
    std::set<std::string> orphaned;
    for (const auto &node : schematic.nodes) {
        // The obsolete segment-deletion code used the literal name "N" for
        // these temporary cut markers. Preserve deliberately placed N1/N2...
        // nodes even while they are not connected yet.
        if (node.ground || node.name != "N")
            continue;
        const bool connected = std::any_of(schematic.wires.begin(), schematic.wires.end(), [&](const Wire &wire) {
            return wire.from.object == node.id || wire.to.object == node.id;
        });
        if (!connected)
            orphaned.insert(node.id);
    }
    std::erase_if(schematic.nodes, [&](const Node &node) { return orphaned.contains(node.id); });
    std::erase_if(schematic.labels, [&](const LabelLayout &label) { return orphaned.contains(label.object); });
}
static QString component_label(const Component &c) {
    if(c.semiconductor.model==SemiconductorModel::piecewise_linear)return "PWL";
    const auto unit=component_unit(c.kind);
    if(unit.empty())return {};
    const auto value=engineering_value(c.value,unit);
    if(c.source.kind==Waveform::dc)return value;
    if(c.source.kind==Waveform::piecewise_linear)return text("wave_table_short");
    return (c.source.kind==Waveform::sine?QString::fromUtf8("∿ "):text("wave_pulse")+" ")+
        value+" · "+engineering_value(c.source.frequency,"Hz");
}
static std::string serialized(const Project &p) {
    std::ostringstream out;
    write_project(p, out);
    return out.str();
}
static QTransform orientation_transform(Orientation o) {
    const unsigned t = o.quarter_turns % 4;
    QTransform transform;
    transform.scale(o.scale * o.scale_x, o.scale * o.scale_y);
    if (o.mirrored)
        transform.scale(-1, 1);
    transform.rotate(int(t) * 90);
    return transform;
}
class PortDot final : public QGraphicsItem {
    QColor color_;
    bool hovered_ = false;

  public:
    explicit PortDot(QColor color, QGraphicsItem *parent) : QGraphicsItem(parent), color_(color) {
        setAcceptHoverEvents(true);
        setCursor(Qt::CrossCursor);
        setZValue(5);
    }
    void setColor(QColor color) {
        if (color_ != color) {
            color_ = color;
            update();
        }
    }
    QRectF boundingRect() const override { return {-7, -7, 14, 14}; }
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) override {
        if (parentItem() && parentItem()->data(4).isValid())
            return; // Junctions are drawn once by the parent, not as component terminals.
        const auto transform = painter->worldTransform();
        const auto center = transform.map(QPointF());
        const double sx = std::hypot(transform.m11(), transform.m12());
        const double sy = std::hypot(transform.m21(), transform.m22());
        const double scale = std::min(sx, sy);
        if (hovered_) {
            painter->setPen(Qt::NoPen);
            auto halo = themed_signal(color_);
            halo.setAlpha(35);
            painter->setBrush(halo);
            painter->save();
            painter->resetTransform();
            painter->drawEllipse(center, 6 * scale, 6 * scale);
            painter->restore();
        }
        auto pen = QPen(themed_signal(color_), 1.2);
        pen.setCosmetic(true);
        painter->save();
        painter->resetTransform();
        painter->setPen(pen);
        painter->setBrush(theme_colors().surface);
        painter->drawEllipse(center, (hovered_ ? 2.8 : 2.0) * scale, (hovered_ ? 2.8 : 2.0) * scale);
        painter->restore();
    }
    void hoverEnterEvent(QGraphicsSceneHoverEvent *) override {
        hovered_ = true;
        update();
    }
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *) override {
        hovered_ = false;
        update();
    }
};
class WireItem final : public QGraphicsPathItem {
  public:
    std::vector<QPointF> junctions;
    void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override {
        auto themed_pen = pen();
        themed_pen.setColor(themed_signal(themed_pen.color()));
        p->setPen(themed_pen);
        p->setBrush(Qt::NoBrush);
        p->drawPath(path());
        p->setBrush(themed_pen.color());
        for (auto point : junctions)
            p->drawEllipse(point, 2.8, 2.8);
        p->setBrush(Qt::NoBrush);
        const int segment = data(wire_segment_role).toInt();
        if (isSelected() && segment > 0 && segment < path().elementCount()) {
            const auto a = path().elementAt(segment - 1), b = path().elementAt(segment);
            auto selected = pen();
            selected.setColor(QColor("#e88b22"));
            selected.setWidthF(3);
            p->setPen(selected);
            p->drawLine(QPointF(a.x, a.y), QPointF(b.x, b.y));
        }
    }
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override {
        if (change == ItemSelectedHasChanged && !value.toBool())
            setData(wire_segment_role, 0);
        return QGraphicsPathItem::itemChange(change, value);
    }
    QPainterPath shape() const override {
        QPainterPathStroker stroke;
        stroke.setWidth(12);
        return stroke.createStroke(path());
    }
    QRectF boundingRect() const override { return path().boundingRect().adjusted(-6, -6, 6, 6); }
};
class Atom final : public QGraphicsItem {
  public:
    std::string id;
    QString name, symbol, value;
    int type = 0;
    unsigned input_count = 2;
    std::vector<std::pair<QString, QPointF>> public_ports;
    std::vector<PublicPort> definition_ports;
    double frame_half_height = 0.0;
    int library_icon_id = -1;
    QImage custom_image;
    std::vector<IconPrimitive> code_icon;
    bool ground = false, separate_labels = false, differential_plot = false;
    Atom(std::string uuid, QString label, QString mark, int category)
        : id(std::move(uuid)), name(label), symbol(mark), type(category) {
        setData(0, q(id));
        setData(1, "atom");
        setData(10, category == 4);
        setFlags(ItemIsSelectable | ItemSendsGeometryChanges);
        setZValue(2);
        setAcceptHoverEvents(true);
    }
    void prepareGeometryChangeForInputs(unsigned count) {
        prepareGeometryChange();
        input_count = count;
    }
    void set_definition(const Definition &definition) {
        custom_image = {};
        if (!definition.appearance.image_png.empty())
            custom_image.loadFromData(QByteArray::fromBase64(
                QByteArray::fromStdString(definition.appearance.image_png)), "PNG");
        library_icon_id = definition.appearance.symbol >= 0
            ? definition.appearance.symbol
            : definition_icon_id(definition.id);
        if (library_icon_id < 0) {
            const auto lower = QString::fromStdString(definition.name).toLower();
            if (lower.contains("three-phase voltage source"))
                library_icon_id = lower.contains("delta") ? 281 : 280;
            else if (lower.contains("three-phase ac voltage controller"))
                library_icon_id = 251;
            else if (lower.contains("ac voltage controller"))
                library_icon_id = 250;
            else if (lower.contains("two-level") || lower.contains("2l") || lower.contains("2-level"))
                library_icon_id = 260;
            else if (lower.contains("three-level") || lower.contains("3l") || lower.contains("3-level") ||
                     lower.contains("npc"))
                library_icon_id = 261;
            else if (lower.contains("full bridge"))
                library_icon_id = 231;
            else if (lower.contains("half bridge"))
                library_icon_id = 230;
            else if (lower.contains("diode bridge"))
                library_icon_id = 210;
            else if (lower.contains("thyristor bridge"))
                library_icon_id = 212;
            else if (lower.contains("igbt"))
                library_icon_id = 201;
            else if (lower.contains("mosfet"))
                library_icon_id = 200;
            else if (lower.contains("bidirectional") && lower.contains("buck-boost"))
                library_icon_id = 223;
            else if (lower.contains("buck-boost"))
                library_icon_id = 222;
            else if (lower.contains("boost"))
                library_icon_id = 221;
            else if (lower.contains("buck"))
                library_icon_id = 220;
        }
        if (definition_ports == definition.ports) {
            int index = 0;
            for (auto *child : childItems()) {
                if (child->data(1).toString() != "port")
                    continue;
                const QPointF model_position = child->data(16).toPointF();
                child->setPos(model_position);
                if (index < int(public_ports.size()))
                    public_ports[size_t(index)].second = model_position;
                ++index;
            }
            return;
        }
        const bool interface_changed = definition_ports.size() != definition.ports.size() ||
            !std::equal(definition_ports.begin(), definition_ports.end(), definition.ports.begin(),
                        [](const PublicPort &left, const PublicPort &right) {
                            return left.id == right.id;
                        });
        prepareGeometryChange();
        definition_ports = definition.ports;
        public_ports.clear();
        for (auto *child : childItems())
            delete child;
        std::vector<const PublicPort *> left, right;
        auto add_public_port = [&](const PublicPort &external, QPointF pt) {
            public_ports.push_back({q(external.name), pt});
            port(q(external.id), pt,
                 QColor(external.domain == Domain::gate     ? "#17866d"
                        : external.domain == Domain::signal ? "#8c67c8"
                                                            : "#146cca"),
                 q(external.name));
        };
        const bool three_phase_source =
            definition.id == "1a963f2c-ceb8-5cce-b927-44d735ec9e80" ||
            definition.id == "eb613164-faf4-5b03-9014-806885fef344" ||
            QString::fromStdString(definition.name).toLower().contains("three-phase voltage source");
        if (three_phase_source) {
            for (const auto &port : definition.ports) {
                if (port.name == "N")
                    left.push_back(&port);
                else
                    right.push_back(&port);
            }
        } else {
            unsigned electrical = 0;
            for (const auto &port : definition.ports) {
                bool on_right = port.direction == Direction::output ||
                                (port.direction == Direction::conserving && electrical++ % 2);
                (on_right ? right : left).push_back(&port);
            }
        }
        input_count = unsigned(std::max(left.size(), right.size()));
        const double spacing = port_spacing();
        auto side = [&](const auto &group, double x) {
            for (size_t n = 0; n < group.size(); ++n) {
                const double y = (double(n) - double(group.size() - 1) / 2.0) * spacing;
                QPointF pt = group[n]->has_position ? QPointF(group[n]->x, group[n]->y) : QPointF(x, y);
                add_public_port(*group[n], pt);
            }
        };
        side(left, -100);
        side(right, 100);
        for (auto *child : childItems())
            for (const auto &port : definition.ports)
                if (child->data(2).toString() == q(port.id))
                    child->setToolTip(q(port.name));
        // A frame is part of the block layout, not a function of a pin's
        // current position. Keep the natural size captured when the item is
        // created; otherwise snapping a pin (or merely rebuilding after a
        // wire edit) feeds its outward endpoint back into the size calculation
        // and makes the block grow on every Undo/Redo cycle.
        if (frame_half_height <= 0.0 || interface_changed)
            frame_half_height = natural_body_half_height();
    }
    double port_spacing() const {
        return input_count > 18 ? 14.0 : input_count > 12 ? 18.0 : input_count > 8 ? 22.0 : 28.0;
    }
    double code_body_half_width() const { return input_count <= 1 ? 38.0 : 70.0; }
    double code_port_distance() const { return input_count <= 1 ? 60.0 : 100.0; }
    double natural_body_half_height() const {
        if (type == 6)
            return std::max(22.0, (std::max(1u, input_count) - 1) * 14.0 + 14.0);
        if (type == 4) {
            double extent = (std::max(1u, input_count) - 1) * port_spacing() / 2.0;
            for (const auto &[port_name, point] : public_ports) {
                (void)port_name;
                // Only side ports determine the block height. A port explicitly
                // placed on the top/bottom edge must not make the body grow on
                // every scene rebuild.
                if (std::abs(point.x()) >= 90.0)
                    extent = std::max(extent, std::abs(point.y()));
            }
            return std::max(42.0, extent + 24.0);
        }
        if (type == 3)
            return std::max(40.0, input_count * 20.0);
        return 40.0;
    }
    double body_half_height() const {
        return type == 4 && frame_half_height > 0.0 ? frame_half_height
                                                    : natural_body_half_height();
    }
    QRectF boundingRect() const override {
        if (type == 6) {
            const double h = natural_body_half_height();
            const double w = code_body_half_width();
            return {-w - 40, -h - 8, 2 * w + 80, 2 * h + 40};
        }
        if (type == 4) {
            double h = body_half_height();
            return {-110, -h - 8, 220, 2 * h + 40};
        }
        if (type == 3) {
            double h = std::max(40.0, input_count * 20.0);
            return {-80, -h - 14, 160, 2 * h + 52};
        }
        if (type == 5)
            return {-34, -18, 108, 36};
        if (type == 2) {
            const double h=std::max(22.0,(std::max(1u,input_count)-1)*10.0+14.0);
            return {-78,-h-31,156,2*h+82};
        }
        return type == 1 ? QRectF(-46, -12, 92, 68) : QRectF(-78, -53, 156, 104);
    }
    QPainterPath shape() const override {
        QPainterPath path;
        if (type == 6) {
            const double h = natural_body_half_height();
            const double w = code_body_half_width();
            path.addRoundedRect(QRectF(-w, -h, 2 * w, 2 * h), 6, 6);
            return path;
        }
        if (type == 4) {
            double h = body_half_height();
            path.addRect(QRectF(-90, -h, 180, 2 * h));
            return path;
        }
        if (type == 5) {
            path.addRoundedRect(QRectF(-30, -14, 60, 28), 4, 4);
            path.addRect(QRectF(30, -10, 42, 20));
        } else if (type == 1) {
            if (ground)
                path.addRect(QRectF(-18, -5, 36, 34));
            else
                path.addEllipse(QPointF(), 7, 7);
            if (!separate_labels && !name.isEmpty())
                path.addRect(QRectF(-45, 31, 90, 20));
        } else if (type == 3) {
            double h = std::max(40.0, input_count * 20.0);
            path.addRect(QRectF(-46, -h, 104, 2 * h));
            if (!separate_labels)
                path.addRect(QRectF(-78, h + 7, 156, 24));
        } else {
            const double gate_h=std::max(22.0,(std::max(1u,input_count)-1)*10.0+14.0);
            path.addRect(type == 2 ? QRectF(-38, -gate_h, 76, 2*gate_h) : QRectF(-29, -28, 58, 56));
            if (!separate_labels)
                path.addRect(QRectF(-77, 28, 154, 22));
            if (!separate_labels && !value.isEmpty())
                path.addRect(QRectF(-77, -51, 154, 20));
        }
        return path;
    }
    void port(const QString &port_name, QPointF location, const QColor &color, const QString &label = {}) {
        auto *item = new PortDot(color, this);
        item->setPos(location);
        item->setData(0, q(id));
        item->setData(1, "port");
        item->setData(2, port_name);
        item->setData(8, label.isEmpty() ? port_name : label);
        item->setData(11, color.name(QColor::HexRgb));
        item->setData(16, location);
        item->setToolTip(label.isEmpty() ? port_name : label);
    }
    void snap_ports_to_grid(double grid) {
        auto snap = [&](QPointF point) {
            const auto coordinate = [grid](double value) { return std::round(value / grid) * grid; };
            return QPointF(coordinate(point.x()), coordinate(point.y()));
        };
        int index = 0;
        for (auto *child : childItems()) {
            if (child->data(1).toString() != "port")
                continue;
            QPointF scene = child->scenePos();
            if (type == 2 || type == 3 || type == 4) {
                const double h = type == 2 ? std::max(22.0,(std::max(1u,input_count)-1)*10.0+14.0)
                    : type == 3 ? std::max(40.0, input_count * 20.0) : body_half_height();
                const QRectF body = type == 2 ? QRectF(-38,-h,76,2*h)
                    : type == 3 ? QRectF(-46, -h, 104, 2 * h) : QRectF(-90, -h, 180, 2 * h);
                const QPointF current = child->pos();
                const std::array<QPointF, 4> edges = {
                    QPointF(body.left(), std::clamp(current.y(), body.top(), body.bottom())),
                    QPointF(body.right(), std::clamp(current.y(), body.top(), body.bottom())),
                    QPointF(std::clamp(current.x(), body.left(), body.right()), body.top()),
                    QPointF(std::clamp(current.x(), body.left(), body.right()), body.bottom())};
                const auto edge = *std::min_element(edges.begin(), edges.end(), [&](QPointF a, QPointF b) {
                    return QLineF(current, a).length() < QLineF(current, b).length();
                });
                QPointF outward;
                if (edge.x() == body.left()) outward = {-1, 0};
                else if (edge.x() == body.right()) outward = {1, 0};
                else if (edge.y() == body.top()) outward = {0, -1};
                else outward = {0, 1};
                const QPointF edge_scene = mapToScene(edge);
                QPointF direction = mapToScene(edge + outward) - edge_scene;
                const double length = std::hypot(direction.x(), direction.y());
                if (length > 1e-9)
                    scene = edge_scene + direction / length * (type == 2 ? grid : grid / 2.0);
            }
            const QPointF local = mapFromScene(snap(scene));
            child->setPos(local);
            if (index < int(public_ports.size()))
                public_ports[size_t(index)].second = local;
            ++index;
        }
    }
    void separate_overlapping_ports(double grid) {
        if (type != 2 && type != 3 && type != 4)
            return;
        const double h = type == 2 ? std::max(22.0,(std::max(1u,input_count)-1)*10.0+14.0)
            : type == 3 ? std::max(40.0, input_count * 20.0) : body_half_height();
        const QRectF body = type == 2 ? QRectF(-38,-h,76,2*h)
            : type == 3 ? QRectF(-46, -h, 104, 2 * h) : QRectF(-90, -h, 180, 2 * h);
        std::vector<QPointF> occupied;
        for (auto *child : childItems()) {
            if (child->data(1).toString() != "port")
                continue;
            const QPointF current = child->pos();
            auto endpoint = [&](QPointF edge, QPointF outward) {
                const QPointF edge_scene = mapToScene(edge);
                QPointF direction = mapToScene(edge + outward) - edge_scene;
                const double length = std::hypot(direction.x(), direction.y());
                if (length <= 1e-9)
                    return edge;
                QPointF target = edge_scene + direction / length * (type == 2 ? grid : grid / 2.0);
                target.setX(std::round(target.x() / grid) * grid);
                target.setY(std::round(target.y() / grid) * grid);
                return mapFromScene(target);
            };
            const std::array<QPointF, 4> projected = {
                endpoint({body.left(), std::clamp(current.y(), body.top(), body.bottom())}, {-1, 0}),
                endpoint({body.right(), std::clamp(current.y(), body.top(), body.bottom())}, {1, 0}),
                endpoint({std::clamp(current.x(), body.left(), body.right()), body.top()}, {0, -1}),
                endpoint({std::clamp(current.x(), body.left(), body.right()), body.bottom()}, {0, 1})};
            const QPointF edge = *std::min_element(projected.begin(), projected.end(), [&](QPointF a, QPointF b) {
                return QLineF(current, a).length() < QLineF(current, b).length();
            });
            const bool vertical_side = edge.x() < body.left() || edge.x() > body.right();
            const QPointF tangent = vertical_side ? QPointF(0, 1) : QPointF(1, 0);
            const double tangent_scale = QLineF(mapToScene(QPointF()), mapToScene(tangent)).length();
            const double step = std::max(1e-6, grid / std::max(1e-9, tangent_scale));
            auto free = [&](QPointF point) {
                return std::none_of(occupied.begin(), occupied.end(), [&](QPointF prior) {
                    return QLineF(point, prior).length() < .5;
                });
            };
            QPointF target = current;
            if (!free(current)) {
                const double low = vertical_side ? body.top() : body.left();
                const double high = vertical_side ? body.bottom() : body.right();
                const double origin = vertical_side ? edge.y() : edge.x();
                std::optional<QPointF> replacement;
                for (int distance = 1; distance <= 64 && !replacement; ++distance)
                    for (int direction : {1, -1}) {
                        const double coordinate = origin + direction * distance * step;
                        if (coordinate < low - 1e-6 || coordinate > high + 1e-6)
                            continue;
                        QPointF candidate = edge;
                        if (vertical_side)
                            candidate.setY(coordinate);
                        else
                            candidate.setX(coordinate);
                        if (free(candidate)) {
                            replacement = candidate;
                            break;
                        }
                    }
                if (!replacement) {
                    std::vector<QPointF> alternatives;
                    const double first_y = std::ceil(body.top() / step) * step;
                    for (double y = first_y; y <= body.bottom() + 1e-6; y += step) {
                        alternatives.push_back({projected[0].x(), y});
                        alternatives.push_back({projected[1].x(), y});
                    }
                    const double first_x = std::ceil(body.left() / step) * step;
                    for (double x = first_x; x <= body.right() + 1e-6; x += step) {
                        alternatives.push_back({x, projected[2].y()});
                        alternatives.push_back({x, projected[3].y()});
                    }
                    std::erase_if(alternatives, [&](QPointF point) { return !free(point); });
                    if (!alternatives.empty())
                        replacement = *std::min_element(alternatives.begin(), alternatives.end(),
                                                        [&](QPointF a, QPointF b) {
                                                            return QLineF(edge, a).length() < QLineF(edge, b).length();
                                                        });
                }
                if (replacement)
                    target = *replacement;
            }
            if (target != current)
                child->setPos(target);
            occupied.push_back(target);
        }
    }
    QVariant itemChange(GraphicsItemChange change, const QVariant &proposed) override {

        return QGraphicsItem::itemChange(change, proposed);
    }
    static void label(QPainter *p, QRectF rect, int flags, const QString &value) {
        auto t = p->worldTransform();
        auto center = t.map(rect.center());
        double scale = std::sqrt(std::abs(t.determinant()));
        p->save();
        p->resetTransform();
        p->translate(center);
        p->scale(scale, scale);
        p->drawText(QRectF(-rect.width() / 2, -rect.height() / 2, rect.width(), rect.height()), flags, value);
        p->restore();
    }
    static void fixed_aspect_icon(QPainter *p, int icon_id, QRectF rect) {
        const auto world = p->worldTransform();
        const auto center = world.map(rect.center());
        const double sx = std::hypot(world.m11(), world.m12());
        const double sy = std::hypot(world.m21(), world.m22());
        const double side = std::min(rect.width(), rect.height());
        const double scale = std::min(sx, sy) * side / 32.0;
        if (side <= 0 || scale <= 0)
            return;
        const double angle = std::atan2(world.m12(), world.m11()) * 180.0 / std::acos(-1.0);
        p->save();
        p->resetTransform();
        p->translate(center);
        p->rotate(angle);
        p->scale(world.determinant() < 0 ? -scale : scale, scale);
        p->translate(-16, -16);
        paint_component_symbol(*p, icon_id, false);
        p->restore();
    }
    static void fixed_converter_marks(QPainter *p, double h) {
        const auto world = p->worldTransform();
        const double sx = std::hypot(world.m11(), world.m12());
        const double sy = std::hypot(world.m21(), world.m22());
        const double scale = std::min(sx, sy);
        if (scale <= 0)
            return;
        const double angle = std::atan2(world.m12(), world.m11()) * 180.0 / std::acos(-1.0);
        auto mark = [&](QPointF local, bool alternating) {
            p->save();
            p->resetTransform();
            p->translate(world.map(local));
            p->rotate(angle);
            p->scale(world.determinant() < 0 ? -scale : scale, scale);
            p->setPen(QPen(theme_colors().text, 1.7, Qt::SolidLine, Qt::RoundCap));
            if (alternating) {
                auto font = p->font();
                font.setPixelSize(19);
                p->setFont(font);
                p->drawText(QRectF(-15, -13, 30, 26), Qt::AlignCenter, "~");
            } else {
                p->drawLine(QPointF(-11, -4), QPointF(11, -4));
                p->drawLine(QPointF(-11, 4), QPointF(11, 4));
            }
            p->restore();
        };
        mark({-48, -h + 24}, true);
        mark({48, h - 24}, false);
    }
    static void fixed_aspect_image(QPainter *p, const QImage &image, QRectF rect) {
        if (image.isNull())
            return;
        const auto world = p->worldTransform();
        const auto center = world.map(rect.center());
        const double sx = std::hypot(world.m11(), world.m12());
        const double sy = std::hypot(world.m21(), world.m22());
        const double scale = std::min(sx, sy);
        QSizeF size = image.size();
        size.scale(rect.size() * scale, Qt::KeepAspectRatio);
        p->save();
        p->resetTransform();
        p->translate(center);
        p->rotate(std::atan2(world.m12(), world.m11()) * 180.0 / std::acos(-1.0));
        if (world.determinant() < 0)
            p->scale(-1, 1);
        p->drawImage(QRectF(-size.width() / 2, -size.height() / 2, size.width(), size.height()), image);
        p->restore();
    }
    void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override {
        const bool channel_highlight = data(channel_highlight_role).toBool();
        auto main_pen = QPen(channel_highlight ? QColor("#b34cce")
                             : isSelected()    ? QColor("#e88b22")
                                               : theme_colors().text,
                             channel_highlight ? 3 : 2);
        main_pen.setCosmetic(true);
        p->setPen(main_pen);
        p->setBrush(theme_colors().surface);
        if (type == 6) {
            const double h = natural_body_half_height();
            const double w = code_body_half_width();
            p->setBrush(theme_colors().canvas);
            p->drawRoundedRect(QRectF(-w, -h, 2 * w, 2 * h), 6, 6);
            auto title_font = p->font();
            title_font.setBold(true);
            p->setFont(title_font);
            if(code_icon.empty())
                label(p, QRectF(-18, -15, 36, 30), Qt::AlignCenter, QString("{C}"));
            else
                paint_code_icon(*p,code_icon,QRectF(-20,-18,40,36));
            title_font.setBold(false);
            title_font.setPointSize(8);
            p->setFont(title_font);
            for (auto *child : childItems()) {
                if (child->data(1).toString() != "port")
                    continue;
                const QPointF point = child->pos();
                const bool left = point.x() < 0;
                p->drawLine(point, QPointF(left ? -w : w, std::clamp(point.y(), -h, h)));
                if (input_count > 1) {
                    const double caption_width = w - 22.0;
                    const auto caption = QFontMetricsF(title_font).elidedText(
                        child->data(8).toString(), Qt::ElideRight, caption_width);
                    label(p, QRectF(left ? -w + 4 : 18, point.y() - 9, caption_width, 18),
                          left ? Qt::AlignLeft | Qt::AlignVCenter : Qt::AlignRight | Qt::AlignVCenter,
                          caption);
                }
            }
            return;
        }
        if (type == 4) {
            double h = body_half_height();
            p->setBrush(theme_colors().canvas);
            p->drawRoundedRect(QRectF(-90, -h, 180, 2 * h), 6, 6);
            auto font = p->font();
            font.setPointSize(8);
            p->setFont(font);
            for (auto *child : childItems()) {
                if (child->data(1).toString() != "port")
                    continue;
                const auto port_name = child->data(8).toString();
                const auto point = child->pos();
                const double vertical_edge_distance = std::min(std::abs(point.x() + 90.0),
                                                               std::abs(point.x() - 90.0));
                const double horizontal_edge_distance = std::min(std::abs(point.y() + h),
                                                                 std::abs(point.y() - h));
                const bool horizontal_side = vertical_edge_distance <= horizontal_edge_distance;
                if (horizontal_side) {
                    const bool left = point.x() < 0;
                    p->drawLine(point, QPointF(left ? -90 : 90, std::clamp(point.y(), -h, h)));
                    label(p, QRectF(left ? -86 : 50, point.y() - 10, 36, 20),
                          left ? Qt::AlignLeft | Qt::AlignVCenter : Qt::AlignRight | Qt::AlignVCenter,
                          QFontMetricsF(font).elidedText(port_name, Qt::ElideRight, 34));
                } else {
                    const bool top = point.y() < 0;
                    p->drawLine(point, QPointF(std::clamp(point.x(), -90.0, 90.0), top ? -h : h));
                    label(p, QRectF(point.x() - 24, top ? -h + 3 : h - 21, 48, 18),
                          Qt::AlignHCenter | (top ? Qt::AlignTop : Qt::AlignBottom),
                          QFontMetricsF(font).elidedText(port_name, Qt::ElideRight, 46));
                }
            }
            if (!code_icon.empty()) {
                const int icon_h = int(std::min(112.0, std::max(58.0, 2.0 * h - 34.0)));
                paint_code_icon(*p,code_icon,QRectF(-44,-icon_h/2,88,icon_h));
            } else if (!custom_image.isNull()) {
                const int image_h = int(std::min(112.0, std::max(58.0, 2.0 * h - 34.0)));
                fixed_aspect_image(p, custom_image, QRectF(-44, -image_h / 2, 88, image_h));
            } else if (library_icon_id >= 0) {
                if (library_icon_id >= 210 && library_icon_id <= 213) {
                    // The diagonal is a structural separator of the converter,
                    // not part of its fixed-size symbol. It follows the block
                    // geometry and reaches the frame at both ends.
                    p->setPen(main_pen);
                    p->drawLine(QPointF(-70, h), QPointF(70, -h));
                    fixed_converter_marks(p, h);
                } else {
                    const int icon_h = int(std::min(112.0, std::max(58.0, 2.0 * h - 34.0)));
                    fixed_aspect_icon(p, library_icon_id, QRectF(-44, -icon_h / 2, 88, icon_h));
                }
            } else {
                // Unknown subcircuits still get a neutral schematic mark. Never
                // substitute their (often long) name inside the already labelled block.
                p->setPen(QPen(theme_colors().text, 1.5));
                p->drawRect(QRectF(-30, -18, 22, 16));
                p->drawRect(QRectF(8, 2, 22, 16));
                p->drawLine(QPointF(-8, -10), QPointF(8, 10));
            }
            return;
        }
        if (type == 3) {
            double h = std::max(40.0, input_count * 20.0);
            p->setBrush(theme_colors().surface);
            auto box_pen = QPen(isSelected() ? QColor("#3a7fe0") : theme_colors().border, 1.5);
            box_pen.setCosmetic(true);
            p->setPen(box_pen);
            p->drawRoundedRect(QRectF(-46, -h, 104, 2 * h), 7, 7);
            p->setPen(QPen(theme_colors().grid, 1));
            p->drawLine(-24, 17, -24, -17);
            p->drawLine(-24, 17, 37, 17);
            if (differential_plot) {
                p->setBrush(Qt::NoBrush);
                p->setPen(QPen(QColor("#5f7cff"), 2));
                QPainterPath positive(QPointF(-20, -10));
                positive.lineTo(-5, -10);
                positive.lineTo(3, 10);
                positive.lineTo(18, 10);
                positive.lineTo(26, -10);
                positive.lineTo(38, -10);
                p->drawPath(positive);
                p->setPen(QPen(QColor("#e05263"), 2));
                QPainterPath negative(QPointF(-20, 10));
                negative.lineTo(-5, 10);
                negative.lineTo(3, -10);
                negative.lineTo(18, -10);
                negative.lineTo(26, 10);
                negative.lineTo(38, 10);
                p->drawPath(negative);
                // Alternate the foreground trace at consecutive crossings:
                // red on the rising edge, blue on the falling edge.
                p->setPen(QPen(QColor("#5f7cff"), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p->drawLine(QPointF(18, 10), QPointF(26, -10));
            } else {
                p->setPen(QPen(theme_colors().signal, 2));
                QPainterPath curve;
                curve.moveTo(-20, 12);
                curve.cubicTo(-6, 10, -4, -15, 10, -11);
                curve.cubicTo(25, -8, 20, 7, 36, -4);
                p->drawPath(curve);
                p->setPen(QPen(theme_colors().text, 1.2));
                p->drawLine(39, 20, 39, 25);
                p->drawLine(32, 25, 46, 25);
                p->drawLine(35, 29, 43, 29);
                p->drawLine(38, 33, 40, 33);
            }
            if(!code_icon.empty()) {
                p->fillRect(QRectF(-22,-28,68,62),theme_colors().surface);
                paint_code_icon(*p,code_icon,QRectF(-18,-24,60,54));
            }
            const auto children = childItems();
            for (unsigned i = 1; i <= input_count; ++i) {
                const QString port_id = differential_plot
                    ? QString(i % 2 ? "p" : "n") + QString::number((i + 1) / 2)
                    : QString("in") + QString::number(i);
                const auto found = std::find_if(children.begin(), children.end(), [&](QGraphicsItem *child) {
                    return child->data(1).toString() == "port" && child->data(2).toString() == port_id;
                });
                if (found == children.end())
                    continue;
                auto *pin = *found;
                const auto point = pin->pos();
                p->setPen(QPen(theme_colors().signal, 1.4));
                const QRectF body(-46, -h, 104, 2 * h);
                const double left_distance = std::abs(point.x() - body.left());
                const double right_distance = std::abs(point.x() - body.right());
                const double top_distance = std::abs(point.y() - body.top());
                const double bottom_distance = std::abs(point.y() - body.bottom());
                const double nearest = std::min({left_distance, right_distance, top_distance, bottom_distance});
                const QString port_label = differential_plot
                    ? ((i % 2 ? QString("+") : QString("−")) + QString::number((i + 1) / 2))
                    : QString::number(i);
                if (nearest == left_distance) {
                    p->drawLine(point, QPointF(body.left(), point.y()));
                    label(p, QRectF(body.left() + 1, point.y() - 9, 20, 18), Qt::AlignCenter, port_label);
                } else if (nearest == right_distance) {
                    p->drawLine(point, QPointF(body.right(), point.y()));
                    label(p, QRectF(body.right() - 21, point.y() - 9, 20, 18), Qt::AlignCenter, port_label);
                } else if (nearest == top_distance) {
                    p->drawLine(point, QPointF(point.x(), body.top()));
                    label(p, QRectF(point.x() - 10, body.top() + 1, 20, 18), Qt::AlignCenter, port_label);
                } else {
                    p->drawLine(point, QPointF(point.x(), body.bottom()));
                    label(p, QRectF(point.x() - 10, body.bottom() - 19, 20, 18), Qt::AlignCenter, port_label);
                }
            }
            p->setPen(theme_colors().text);
            if (!separate_labels)
                label(p, QRectF(-78, h + 7, 156, 24), Qt::AlignCenter, name);
            return;
        }
        if (type == 5) {
            const Domain domain = symbol == "G" ? Domain::gate
                                  : symbol == "S" ? Domain::signal : Domain::electrical;
            const QColor color = themed_signal(domain_color(domain));
            p->setBrush(theme_colors().canvas);
            p->setPen(QPen(color, isSelected() ? 2.4 : 1.6));
            QPainterPath tag;
            tag.moveTo(-28, -14);
            tag.lineTo(20, -14);
            tag.lineTo(32, 0);
            tag.lineTo(20, 14);
            tag.lineTo(-28, 14);
            tag.closeSubpath();
            p->drawPath(tag);
            p->drawLine(-42, 0, -28, 0);
            p->setPen(theme_colors().text);
            auto font = p->font();
            font.setBold(true);
            p->setFont(font);
            if(code_icon.empty())label(p, QRectF(-26, -11, 50, 22), Qt::AlignCenter, symbol);
            else paint_code_icon(*p,code_icon,QRectF(-23,-11,42,22));
            if (!separate_labels) {
                font.setBold(false);
                p->setFont(font);
                label(p, QRectF(36, -10, 64, 20), Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetricsF(font).elidedText(name, Qt::ElideRight, 60));
            }
            return;
        }
        if (type == 1) {
            if (ground) {
                p->drawLine(0, 0, 0, 15);
                p->drawLine(-16, 15, 16, 15);
                p->drawLine(-10, 21, 10, 21);
                p->drawLine(-4, 27, 4, 27);
            } else {
                p->setBrush(channel_highlight ? QColor("#b34cce") : theme_colors().text);
                if (data(4).toInt() >= 3)
                    p->drawEllipse(QPointF(0, 0), 3, 3);
                else if (isSelected() || data(4).toInt() == 0) {
                    p->setBrush(Qt::NoBrush);
                    p->drawRect(QRectF(-4, -4, 8, 8));
                }
            }
            if (!separate_labels)
                label(p, QRectF(-45, 31, 90, 20), Qt::AlignCenter, name);
            return;
        }
        if (type == 2) {
            const double h=std::max(22.0,(std::max(1u,input_count)-1)*10.0+14.0);
            p->setBrush(theme_colors().gate_fill);
            p->drawRoundedRect(QRectF(-38, -h, 76, 2*h), 7, 7);
            if(code_icon.empty())label(p, QRectF(-38, -h, 76, 2*h), Qt::AlignCenter, symbol.isEmpty() ? QString("Gate") : symbol);
            else paint_code_icon(*p,code_icon,QRectF(-22,-std::min(22.0,h-3),44,2*std::min(22.0,h-3)));
            unsigned index=0;
            for(auto *child:childItems()) {
                if(child->data(1).toString()!="port")continue;
                const QPointF pin=child->pos();
                const QRectF body(-38,-h,76,2*h);
                const std::array<QPointF,4> edges={QPointF(body.left(),std::clamp(pin.y(),body.top(),body.bottom())),
                    QPointF(body.right(),std::clamp(pin.y(),body.top(),body.bottom())),
                    QPointF(std::clamp(pin.x(),body.left(),body.right()),body.top()),
                    QPointF(std::clamp(pin.x(),body.left(),body.right()),body.bottom())};
                const auto edge=*std::min_element(edges.begin(),edges.end(),[&](QPointF a,QPointF b){return QLineF(pin,a).length()<QLineF(pin,b).length();});
                p->drawLine(edge,pin);
                if(input_count>1) {
                    QRectF number(edge.x()-9,edge.y()-9,18,18);
                    if(edge.x()==body.left())number.translate(12,0);
                    else if(edge.x()==body.right())number.translate(-12,0);
                    else if(edge.y()==body.top())number.translate(0,12);
                    else number.translate(0,-12);
                    label(p,number,Qt::AlignCenter,QString::number(index));
                }
                ++index;
            }
        } else {
            p->drawLine(-60, 0, -27, 0);
            p->drawLine(27, 0, 60, 0);
            p->save();
            auto port_font = p->font();
            port_font.setPointSize(8);
            p->setFont(port_font);
            if (symbol != "R" && symbol != "L" && symbol != "C" && symbol != "S") {
                const auto left = symbol == "V" || symbol == "I" ? QString("+") : QString("p");
                const auto right = symbol == "V" || symbol == "I" ? QString("-") : QString("n");
                label(p, QRectF(-61, 4, 18, 16), Qt::AlignLeft, left);
                label(p, QRectF(44, 4, 18, 16), Qt::AlignRight, right);
            }
            p->restore();
            if(!code_icon.empty())
                paint_code_icon(*p,code_icon,QRectF(-25,-25,50,50));
            else if (symbol == "R")
                p->drawRect(QRectF(-27, -12, 54, 24));
            else if (symbol == "C") {
                p->drawLine(-27, 0, -7, 0);
                p->drawLine(7, 0, 27, 0);
                p->drawLine(-7, -22, -7, 22);
                p->drawLine(7, -22, 7, 22);
            } else if (symbol == "L") {
                for (int i = 0; i < 4; ++i)
                    p->drawArc(QRectF(-28 + i * 14, -14, 14, 28), 0, 180 * 16);
            } else if (symbol == "IGBT") {
                p->drawLine(-27, 0, -16, 0);
                p->drawLine(-16, -18, -16, 18);
                p->drawLine(16, -16, 16, 16);
                p->drawLine(16, 0, 27, 0);
                p->drawLine(-22, -18, 22, -18);
                p->drawLine(-22, -24, 22, -24);
                p->drawLine(0, -40, 0, -24);
                QPolygonF arrow;
                arrow << QPointF(4, -4) << QPointF(14, -10) << QPointF(12, 2);
                p->drawPolygon(arrow);
            } else if (symbol == "S") {
                p->drawLine(-27, 0, 20, -18);
                p->drawLine(0, -40, 0, -26);
            } else if (symbol == "D" || symbol == "T") {
                QPolygonF triangle;
                triangle << QPointF(-20, -18) << QPointF(-20, 18) << QPointF(20, 0);
                p->drawPolygon(triangle);
                p->drawLine(20, -18, 20, 18);
                p->drawLine(-27, 0, -20, 0);
                p->drawLine(20, 0, 27, 0);
                if (symbol == "T") {
                    p->drawLine(0, -40, 0, -30);
                    p->drawLine(0, -30, 20, -12);
                }
            } else {
                p->drawEllipse(QRectF(-27, -27, 54, 54));
                label(p, QRectF(-27, -27, 54, 54), Qt::AlignCenter, symbol);
            }
        }
        if (!separate_labels)
            label(p, QRectF(-77, 28, 154, 22), Qt::AlignCenter, name);
        if (!separate_labels && !value.isEmpty())
            label(p, QRectF(-77, -51, 154, 20), Qt::AlignCenter, value);
    }
};
bool EditorWindow::edit_text_at(QPoint point) {
    if (!editing_allowed())
        return false;
    for (auto *item : canvas_->items(point)) {
        if (item->data(1).toString() != "label")
            continue;
        const auto id = item->data(0).toString().toStdString();
        const auto role = item->data(2).toString();
        const auto &spec = component_specs_.at(object_type(project(), id));
        for (auto entry : spec.value("fields").toArray()) {
            auto field = entry.toObject();
            if (field.value("inline").toString() != role)
                continue;
            if(!property_visible(project(),id,field))continue;
            if (field.contains("inlinePart")) {
                auto *a = static_cast<Atom *>(atoms_.at(id));
                auto parts = a->value.split(" · ");
                int part = field.value("inlinePart").toInt();
                if (part < 0 || part >= parts.size())
                    continue;
                QFontMetricsF metrics(canvas_->font());
                double total = metrics.horizontalAdvance(a->value), left = -total / 2;
                for (int i = 0; i < part; ++i)
                    left += metrics.horizontalAdvance(parts[i] + " · ");
                double px = item->mapFromScene(canvas_->mapToScene(point)).x();
                if (total > 0 && (px < left - 2 || px > left + metrics.horizontalAdvance(parts[part]) + 2))
                    continue;
            }
            edit_inline(id, field, canvas_->mapFromScene(item->sceneBoundingRect()).boundingRect());
            return true;
        }
        select_object(id);
        if(auto samples=property_editors_.find("source_points");samples!=property_editors_.end()&&samples->second->isVisible())samples->second->setFocus();
        return true;
    }
    return false;
}
void EditorWindow::update_labels() {
    std::set<std::string> present;
    for (const auto &[id, item] : atoms_) {
        auto *atom = static_cast<Atom *>(item);
        if (!atom->isVisible())
            continue;
        for (const QString role : {QString("name"), QString("value")}) {
            if (atom->type == 1 && !atom->ground)
                continue;
            const auto contents = role == "name" ? atom->name : atom->value;
            if (contents.isEmpty())
                continue;
            const auto key = id + "/" + role.toStdString();
            present.insert(key);
            LabelItem *label;
            if (labels_.count(key))
                label = static_cast<LabelItem *>(labels_.at(key));
            else {
                label = new LabelItem(q(id), role);
                labels_[key] = label;
                canvas_->scene()->addItem(label);
            }
            label->set_text(contents);
            label->setData(10, atom->type == 4);
            label->setToolTip(text("label_hint"));
            const QPointF anchor = role == "value"   ? QPointF(0, -41)
                                   : atom->type == 1 ? QPointF(0, 41)
                                   : atom->type == 3 ? QPointF(0, std::max(40., atom->input_count * 20.) + 20)
                                   : atom->type == 4 ? QPointF(0, atom->body_half_height() + 20)
                                   : atom->type == 6 ? QPointF(0, atom->natural_body_half_height() + 20)
                                                     : QPointF(0, 39);
            label->setData(5, anchor);
            if (canvas_->editing_gesture() && label->isSelected() && !atom->isSelected())
                continue;
            LabelLayout layout;
            bool custom_layout=false;
            for (const auto &l : project().labels)
                if (l.object == id && l.role == role.toStdString()) {
                    layout = l;
                    custom_layout=true;
                }
            auto position=anchor+QPointF(layout.x,layout.y);
            if(!custom_layout&&atom->type==0) {
                const auto direction=atom->mapToScene(anchor)-atom->scenePos();
                const double clearance=40+label->boundingRect().width()/2;
                if(std::abs(direction.x())>std::abs(direction.y())&&std::abs(direction.x())<clearance)
                    position=atom->mapFromScene(atom->scenePos()+QPointF(std::copysign(clearance,direction.x()),direction.y()));
            }
            label->setPos(atom->mapToScene(position));
            QTransform transform;
            transform.rotate(layout.orientation.quarter_turns * 90);
            if (layout.orientation.mirrored)
                transform.scale(-1, 1);
            label->setTransform(transform);
        }
    }
    for (auto it = labels_.begin(); it != labels_.end();)
        if (!present.count(it->first)) {
            delete it->second;
            it = labels_.erase(it);
        } else
            ++it;
}
bool EditorWindow::commit_label_positions() {
    std::vector<LabelLayout> changed;
    for (const auto &[key, item] : labels_)
        if (item->isSelected()) {
            const auto id = item->data(0).toString().toStdString();
            auto atom = atoms_.find(id);
            if (atom == atoms_.end() || atom->second->isSelected())
                continue;
            auto pos = atom->second->mapFromScene(item->pos()) - item->data(5).toPointF();
            auto t = item->transform();
            LabelLayout layout{
                id,
                item->data(2).toString().toStdString(),
                pos.x(),
                pos.y(),
                {static_cast<unsigned>(
                     (static_cast<int>(std::lround(std::atan2(t.m12(), t.m11()) / (std::acos(-1.) / 2))) +
                      4) %
                     4),
                 t.determinant() < 0}};
            if (layout.orientation.mirrored)
                layout.orientation.quarter_turns = (layout.orientation.quarter_turns + 2) % 4;
            auto old = std::find_if(project().labels.begin(), project().labels.end(), [&](const auto &l) {
                return l.object == layout.object && l.role == layout.role;
            });
            if (old == project().labels.end()
                    ? (layout.x != 0 || layout.y != 0 || layout.orientation != Orientation{})
                    : *old != layout)
                changed.push_back(layout);
        }
    if (changed.empty())
        return false;
    document_->apply("Move labels", [&](Project &p) {
        for (const auto &layout : changed) {
            std::erase_if(p.labels,
                          [&](const auto &l) { return l.object == layout.object && l.role == layout.role; });
            p.labels.push_back(layout);
        }
    });
    refresh(false);
    return true;
}
bool EditorWindow::transform_labels(int turns, bool mirror) {
    bool found = false;
    for (const auto &[key, item] : labels_)
        if (item->isSelected()) {
            const auto id = item->data(0).toString().toStdString();
            auto atom = atoms_.find(id);
            if (atom == atoms_.end() || atom->second->isSelected())
                continue;
            auto transform = item->transform();
            if (mirror)
                transform.scale(-1, 1);
            else
                transform.rotate(turns * 90);
            item->setTransform(transform);
            found = true;
        }
    if (found)
        commit_label_positions();
    return found;
}
QGraphicsItem *EditorWindow::make_atom_preview(const Project &fragment) {
    auto *group = new QGraphicsItemGroup;
    auto add = [&](const auto &o, int type, const QString &symbol, const QString &value) {
        auto *a = new Atom(o.id, q(o.name), symbol, type);
        a->value = value;
        a->setPos(o.x, o.y);
        a->setTransform(orientation_transform(o.orientation));
        group->addToGroup(a);
        return a;
    };
    for (const auto &c : fragment.components) {
        auto *a = add(c, 0, q(kind_name(c.kind)), component_label(c));
        a->port("p", {-60, 0}, QColor("#146cca"));
        a->port("n", {60, 0}, QColor("#146cca"));
        if (gate_controlled(c.kind))
            a->port("gate", {0, -40}, QColor("#17866d"));
        if (c.kind == Kind::voltage_probe || c.kind == Kind::current_probe)
            a->port("out", {0, -40}, QColor("#8c67c8"));
    }
    for (const auto &n : fragment.nodes) {
        auto copy = n;
        if (!copy.ground)
            copy.name.clear();
        auto *a = add(copy, 1, {}, {});
        a->ground = n.ground;
        a->port("node", {0, 0}, QColor("#146cca"));
    }
    for (const auto &t : fragment.tags) {
        auto *a = add(t, 5, tag_symbol(t.domain), {});
        a->port("io", {0, 0}, domain_color(t.domain));
    }
    for (const auto &g : fragment.patterns) {
        auto *a = add(g, 2, g.script ? QString("Code") : g.pwm ? QString("PWM") : QString(),
                      g.script ? QString()
                               : g.pwm ? QString::number(g.frequency) + " Hz · " + QString::number(g.duty * 100) + " %"
                                       : QString());
        a->port("out", {60, 0}, QColor("#17866d"));
    }
    for (const auto &g : fragment.plots) {
        auto *a = add(g, 3, {}, {});
        a->differential_plot = g.differential;
        a->input_count = g.differential ? g.inputs * 2 : g.inputs;
        for (unsigned i = 1; i <= g.inputs; ++i) {
            if (g.differential) {
                const double y = (2.0 * static_cast<double>(i) - static_cast<double>(a->input_count) - 1.0) * 10.0;
                a->port("p" + QString::number(i), {-60, y}, QColor("#8c67c8"), "+");
                a->port("n" + QString::number(i), {-60, y + 20.0}, QColor("#8c67c8"), "−");
            } else {
                const double y = (static_cast<double>(i) - (static_cast<double>(g.inputs) + 1.0) / 2.0) * 20.0;
                a->port("in" + QString::number(i), {-60, y},
                        QColor("#8c67c8"));
            }
        }
    }
    for (const auto &block : fragment.code_blocks) {
        auto *a = add(block, 6, QString("{C}"), {});
        a->code_icon = block.icon;
        a->input_count = unsigned(std::max(block.inputs.size(), block.outputs.size()));
        auto append = [&](const std::vector<CodePort> &ports, bool input) {
            for (size_t index = 0; index < ports.size(); ++index) {
                const auto &port = ports[index];
                const double y = (double(index) - double(ports.size() - 1) / 2.0) * 28.0;
                const QString scalar = port.type == SignalScalarType::boolean ? QString("bool") : QString("double");
                const QString unit = port.unit.empty() ? QString() : QString(" [") + q(port.unit) + "]";
                const double x = a->code_port_distance();
                a->port(q(port.id), {input ? -x : x, y},
                        port.type == SignalScalarType::boolean ? QColor("#17866d") : QColor("#8c67c8"),
                        q(port.name) + ": " + scalar + unit);
            }
        };
        append(block.inputs, true);
        append(block.outputs, false);
    }
    for (const auto &instance : fragment.instances)
        add(instance, 4, {}, {})->set_definition(definition(fragment, instance.definition));
    for(const auto &appearance:fragment.object_icons)
        for(auto *child:group->childItems())
            if(auto *a=dynamic_cast<Atom *>(child);a&&a->id==appearance.object)
                a->code_icon=appearance.primitives;
    auto position = [&](const Endpoint &endpoint) -> std::optional<QPointF> {
        for (auto *child : group->childItems())
            if (auto *a = dynamic_cast<Atom *>(child); a && a->id == endpoint.object)
                for (auto *port : a->childItems())
                    if (port->data(2).toString() == q(endpoint.port))
                        return port->mapToItem(group, QPointF());
        return {};
    };
    std::vector<QRectF> obstacles;
    for (auto *child : group->childItems())
        obstacles.push_back(child->mapRectToParent(QRectF(-38, -28, 76, 56)));
    for (const auto &wire : fragment.wires) {
        auto a = position(wire.from), b = position(wire.to);
        if (!a || !b)
            continue;
        auto path =
            wire.bends.empty() ? orthogonal_route(*a, *a, *b, *b, obstacles) : manual_route(*a, *b, wire.bends);
        auto *item = new QGraphicsPathItem(path);
        const auto line = wire.line == WireLine::dash ? Qt::DashLine : Qt::SolidLine;
        item->setPen(QPen(wire.color.empty() ? QColor("#146cca") : QColor(QString::fromStdString(wire.color)),
                          wire.width, line));
        item->setZValue(-2);
        group->addToGroup(item);
    }
    const auto children = group->childItems();
    for (auto *child : children)
        if (auto *a = dynamic_cast<Atom *>(child)) {
            a->separate_labels = true;
            for (const auto &role : {QString("name"), QString("value")}) {
                const auto content = role == "name" ? a->name : a->value;
                if (content.isEmpty())
                    continue;
                QPointF anchor(0, role == "value" ? -41
                                  : a->type == 1  ? 41
                                  : a->type == 3  ? std::max(40., a->input_count * 20.) + 20
                                  : a->type == 4  ? a->body_half_height() + 20
                                  : a->type == 6  ? a->natural_body_half_height() + 20
                                                  : 39);
                LabelLayout layout;
                for (const auto &stored : fragment.labels)
                    if (stored.object == a->id && stored.role == role.toStdString())
                        layout = stored;
                auto *label = new LabelItem(q(a->id), role);
                label->set_text(content);
                label->setPos(a->mapToParent(anchor + QPointF(layout.x, layout.y)));
                QTransform rotation;
                rotation.rotate(layout.orientation.quarter_turns * 90);
                if (layout.orientation.mirrored)
                    rotation.scale(-1, 1);
                label->setTransform(rotation);
                group->addToGroup(label);
            }
        }
    return group;
}
void EditorWindow::set_placement_preview() {
    if (!paste_fragment_) {
        Project empty;
        empty.id = new_uuid();
        empty.wired = true;
        Document fragment(empty);
        if (placing_ < 100)
            fragment.add_component(static_cast<Kind>(placing_), 0, 0);
        else if (placing_ == 102 || placing_ == 104 || placing_ == 105) {
            fragment.add_pattern(0, 0);
            if (placing_ == 104)
                fragment.apply("PWM", [](Project &p) {
                    p.patterns[0].pwm = true;
                    p.patterns[0].name = "PWM";
                });
            if (placing_ == 105)
                fragment.apply("Gate script", [](Project &p) {
                    p.patterns[0].script = true;
                    p.patterns[0].name = "Gate script";
                    p.patterns[0].code = "pwm(1000, 0.5, 0)";
                });
        } else if (placing_ == 106)
            fragment.apply("Tag", [](Project &p) { p.tags.push_back({new_uuid(), "TAG", 0, 0, Domain::gate}); });
        else if (const auto *preset = signal_preset(placing_)) {
            const auto name = component_actions_.at(preset->placement_id)->text().toStdString();
            fragment.apply("Add signal preset", [preset, name](Project &p) {
                p.code_blocks.push_back(make_signal_preset(*preset, name, 0, 0));
            });
        } else if (placing_ == 108)
            fragment.apply("Code block", [](Project &p) {
                CodeBlock block;
                block.id = new_uuid();
                block.name = text("code_block").toStdString();
                block.code = "out = 0;";
                block.icon = default_code_icon(108);
                block.outputs.push_back({new_uuid(), "out", "", SignalScalarType::real, 0});
                p.code_blocks.push_back(std::move(block));
            });
        else if (placing_ == 103 || placing_ == 107)
            fragment.add_plot(0, 0, text(placing_ == 107 ? "differential_plot" : "plot").toStdString(),
                              placing_ == 107);
        else
            fragment.add_node(placing_ == 100, 0, 0);
        paste_fragment_ = fragment.project();
    }
    canvas_->set_ghost(make_atom_preview(*paste_fragment_));
}
void EditorWindow::cancel_placement() {
    paste_fragment_.reset();
    placing_ = -1;
    if (library_)
        library_->clearSelection();
    if (banner_)
        banner_->setText(text("hint"));
}
void EditorWindow::place_at(QPointF point) {
    if (running() || !paste_fragment_)
        return;
    const auto *preset = signal_preset(placing_);
    if (placing_ == 108 || preset) {
        canvas_->set_ghost(nullptr);
        cancel_placement();
        (void)add_code_block(point, preset);
        canvas_->setFocus();
        return;
    }
    try {
        auto ids = document_->paste(*paste_fragment_, point.x(), point.y());
        canvas_->set_ghost(nullptr);
        cancel_placement();
        canvas_->scene()->clearSelection();
        selected_ = ids.empty() ? "" : ids.front();
        refresh();
        auto_connect_nearby_pins();
        for (const auto &id : ids)
            if (atoms_.count(id))
                atoms_.at(id)->setSelected(true);
        canvas_->setFocus();
    } catch (const std::exception &e) {
        show_error(e);
    }
}
EditorWindow::EditorWindow(const QString &language, const QString &recovery_dir)
    : recovery_dir_(recovery_dir) {
    init_language(language);
    auto ui_font = font();
    ui_font.setPointSize(10);
    setFont(ui_font);
    resize(1360, 900);
    setMinimumSize(1050, 720);
    if (recovery_dir_.isEmpty())
        recovery_dir_ = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QSettings appearance(recovery_dir_ + "/ui.ini", QSettings::IniFormat);
    apply_theme(appearance.value("dark_theme", false).toBool());
    build_ui();
    canvas_->set_grid_size(appearance.value("grid_size", 20).toDouble());
    canvas_->set_grid_style(static_cast<Canvas::GridStyle>(
        std::clamp(appearance.value("grid_style", int(Canvas::GridStyle::dots)).toInt(), 0, 2)));
    canvas_->set_grid_line_width(appearance.value("grid_line_width", 1.0).toDouble());
    canvas_->set_grid_dot_size(appearance.value("grid_dot_size", 1.0).toDouble());
    connect(&watcher_, &QFutureWatcher<Outcome>::finished, this, [this] { finish_simulation(); });
    autosave_timer_ = new QTimer(this);
    connect(autosave_timer_, &QTimer::timeout, this, [this] { autosave(); });
    update_autosave_timer();
    auto *progress_timer = new QTimer(this);
    connect(progress_timer, &QTimer::timeout, this, [this] {
        if (running())
            drain_simulation_stream();
        if (running() && !cancel_.load()) {
            if (simulation_progress_) {
                simulation_progress_->setVisible(true);
                if (preparing_.load()) {
                    simulation_progress_->setRange(0, 0);
                    simulation_progress_->setFormat(text("preparing"));
                } else {
                    simulation_progress_->setRange(0, 1000);
                    const double stop = std::max(project().profile.stop, 1e-30);
                    const double progress = std::clamp(simulated_time_.load(std::memory_order_relaxed) / stop, 0.0, 1.0);
                    simulation_progress_->setValue(int(std::lround(progress * 1000.0)));
                    simulation_progress_->setFormat(QString::number(progress * 100.0, 'f', 1) + "%");
                }
            }
            banner_->setText(
                text(paused_.load()      ? "paused"
                     : preparing_.load() ? "preparing"
                                         : "running") +
                "  t = " + QString::number(simulated_time_.load(std::memory_order_relaxed), 'g', 6) + " / " +
                QString::number(project().profile.stop, 'g', 6) + " s · " +
                text("elapsed").arg(simulation_timer_.elapsed() / 1000., 0, 'f', 2));
        }
    });
    progress_timer->start(100);
    new_file();
    if (QFile::exists(recovery_path()))
        banner_->setText(text("recovery_available"));
}
EditorWindow::~EditorWindow() {
    cancel_inline_edit();
    rebuilding_ = true;
    canvas_->cancel_gesture();
    qApp->removeEventFilter(this);
    disconnect(qApp, nullptr, this, nullptr);
    for (auto *child : findChildren<QObject *>())
        disconnect(child, nullptr, this, nullptr);
    disconnect(canvas_->scene(), nullptr, this, nullptr);
    disconnect(&watcher_, nullptr, this, nullptr);
    cancel_ = true;
    watcher_.waitForFinished();
}
void EditorWindow::build_ui() {

    auto *file_menu = menuBar()->addMenu(text("file_menu"));
    auto *edit_menu = menuBar()->addMenu(text("edit_menu"));
    auto *view_menu = menuBar()->addMenu(text("view_menu"));
    auto *help_menu = menuBar()->addMenu(text("help_menu"));
    auto action = [&](QMenu *menu, const char *key, const QKeySequence &shortcut, auto callback) {
        auto *a = new QAction(text(key), this);
        a->setObjectName(QString("action_") + key);
        a->setShortcut(shortcut);
        commands_[key] = a;
        default_shortcuts_[key] = shortcut;
        addAction(a);
        if (menu)
            menu->addAction(a);
        connect(a, &QAction::triggered, this, callback);
        return a;
    };
    action(help_menu, "c_code_reference", {},
           [this] { show_c_code_reference(this); });
    action(file_menu, "new", QKeySequence::New, [this] {
        if (!running() && confirm_discard())
            new_file();
    });
    action(file_menu, "open", QKeySequence::Open, [this] {
        if (running() || !confirm_discard())
            return;
        QSettings settings(recovery_dir_ + "/ui.ini", QSettings::IniFormat);
        auto f = QFileDialog::getOpenFileName(this, text("open"), settings.value("last_project_dir").toString(),
                                              text("project_filter"));
        if (!f.isEmpty())
            open_project(f);
    });
    action(file_menu, "save", QKeySequence::Save, [this] {
        auto f = path_;
        if (f.isEmpty())
            f = QFileDialog::getSaveFileName(this, text("save"), suggested_save_path(), text("project_filter"));
        if (!f.isEmpty())
            save_project(f);
    });
    file_menu->addSeparator();
    action(file_menu, "autosave_settings", {}, [this] { configure_autosave(); });
    action(file_menu, "recover", {}, [this] {
        if (running() || !confirm_discard())
            return;
        if (QFile::exists(recovery_path()))
            recover(recovery_path());
        else
            banner_->setText(text("no_recovery"));
    });
    undo_ = action(edit_menu, "undo", QKeySequence::Undo, [this] { undo(); });
    redo_ = action(edit_menu, "redo", QKeySequence::Redo, [this] { redo(); });
    action(edit_menu, "delete", QKeySequence::Delete, [this] {
        auto *focus = QApplication::focusWidget();
        if (channels_ && (focus == channels_ || channels_->isAncestorOf(focus)))
            remove_scope_point();
        else
            delete_selected();
    });
    action(edit_menu, "rotate", QKeySequence("Space"), [this] { transform_selection(1, false); });
    action(edit_menu, "rotate_back", QKeySequence("Shift+Space"), [this] { transform_selection(-1, false); });
    action(edit_menu, "mirror", QKeySequence("Ctrl+M"), [this] { transform_selection(0, true); });
    action(edit_menu, "scale_up", QKeySequence("Ctrl++"), [this] { scale_selection(1.15); });
    action(edit_menu, "scale_down", QKeySequence("Ctrl+-"), [this] { scale_selection(1.0 / 1.15); });
    action(edit_menu, "scale_reset", {}, [this] { scale_selection(0); });
    action(edit_menu, "copy", QKeySequence::Copy, [this] {
        auto *focus = QApplication::focusWidget();
        if (errors_ && (focus == errors_ || errors_->isAncestorOf(focus)))
            copy_diagnostics();
        else
            copy_selection(false);
    });
    action(edit_menu, "cut", QKeySequence::Cut, [this] { copy_selection(true); });
    action(edit_menu, "paste", QKeySequence::Paste, [this] { paste_selection(); });
    action(edit_menu, "duplicate", QKeySequence("Ctrl+D"), [this] { paste_selection(true); });
    action(edit_menu, "select_all", QKeySequence::SelectAll, [this] {
        for (auto &[id, item] : atoms_)
            item->setSelected(true);
    });
    action(edit_menu, "properties", QKeySequence("Alt+Return"), [this] {
        if (std::any_of(project().code_blocks.begin(), project().code_blocks.end(),
                        [&](const CodeBlock &block) { return block.id == selected_; })) {
            edit_code_block(selected_);
            return;
        }
        fill_inspector();
        if (name_ && name_->isVisible()) {
            name_->setFocus();
            name_->selectAll();
        }
    });
    for (const char *mode : {"left", "right", "top", "bottom", "horizontal", "vertical"})
        action(nullptr, mode, {}, [this, mode] { arrange_selection(mode); });
    auto *grid_action = action(edit_menu, "grid_settings", QKeySequence("G"), [this] { configure_grid(); });
    action(edit_menu, "shortcuts", {}, [this] { show_shortcuts(); });
    auto *search_action = action(edit_menu, "command_search", QKeySequence("Ctrl+Shift+P"), [this] { show_command_search(); });
    auto *search_button = new QToolButton(menuBar());
    search_button->setDefaultAction(search_action);
    search_button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    search_button->setAutoRaise(true);
    search_button->setObjectName("command_search_button");
    const int menu_height = menuBar()->fontMetrics().height() + 18;
    search_button->setFixedSize(search_button->fontMetrics().horizontalAdvance(search_action->text()) + 24,
                                menu_height - 4);
    search_button->setStyleSheet(
        "QToolButton{border:0;background:transparent;padding:5px 12px;border-radius:4px;}"
        "QToolButton:hover{background:palette(highlight);}");
    menuBar()->setCornerWidget(search_button, Qt::TopRightCorner);
    menuBar()->setFixedHeight(menu_height);
    build_hierarchy_actions(edit_menu->addMenu(text("hierarchy")));
    auto *wire_action = action(edit_menu, "connect_tool", QKeySequence("Ctrl+W"), [this] {
        if (running())
            return;
        cancel_placement();
        canvas_->start_connect_mode();
        banner_->setText(text("wire_hint"));
    });
    auto *fit_action = action(view_menu, "fit", QKeySequence("F"), [this] {
        canvas_->fitInView(canvas_->scene()->itemsBoundingRect().adjusted(-90, -90, 90, 90),
                           Qt::KeepAspectRatio);
    });
    action(view_menu, "fit_selection", QKeySequence("Shift+F"), [this] {
        QRectF bounds;
        for (auto *item : canvas_->scene()->selectedItems())
            bounds = bounds.united(item->sceneBoundingRect());
        if (!bounds.isNull())
            canvas_->fitInView(bounds.adjusted(-50, -50, 50, 50), Qt::KeepAspectRatio);
    });
    action(view_menu, "actual_size", QKeySequence("Ctrl+0"), [this] {
        auto center = canvas_->mapToScene(canvas_->viewport()->rect().center());
        canvas_->resetTransform();
        canvas_->centerOn(center);
    });
    action(view_menu, "diagnostics", {}, [this] { bottom_->setCurrentIndex(0); });
    action(view_menu, "scope_tab", {}, [this] { bottom_->setCurrentIndex(1); });
    view_menu->addSeparator();
    auto *theme_action = view_menu->addAction(text("dark_theme"));
    theme_action->setObjectName("dark_theme");
    theme_action->setCheckable(true);
    theme_action->setChecked(dark_theme());
    connect(theme_action, &QAction::toggled, this, [this](bool enabled) {
        QSettings appearance(recovery_dir_ + "/ui.ini", QSettings::IniFormat);
        appearance.setValue("dark_theme", enabled);
        apply_theme(enabled);
        refresh_component_icons();
        refresh_hierarchy();
        choose_channels();
        canvas_->scene()->update();
        for (auto *widget : QApplication::allWidgets())
            if (auto *scope = dynamic_cast<Scope *>(widget))
                scope->refresh_theme();
    });
    auto *simulation_menu = menuBar()->addMenu(text("simulation_menu"));
    auto *continue_action = action(simulation_menu, "continue_state", {}, [this] { continue_simulation(); });
    auto *step_action = action(simulation_menu, "simulation_step", QKeySequence("F10"), [this] { step_simulation(); });
    continue_action->setToolTip(text("continue_state_hint"));
    step_action->setToolTip(text("simulation_step_hint"));
    simulation_menu->addSeparator();
    action(simulation_menu, "snapshot_save", {}, [this] {
        auto path = QFileDialog::getSaveFileName(this, text("snapshot_save"), {}, text("snapshot_filter"));
        if (!path.isEmpty())
            save_simulation_snapshot(path);
    });
    action(simulation_menu, "snapshot_load", {}, [this] {
        auto path = QFileDialog::getOpenFileName(this, text("snapshot_load"), {}, text("snapshot_filter"));
        if (!path.isEmpty())
            load_simulation_snapshot(path);
    });
    simulation_menu->addSeparator();
    action(simulation_menu, "initial_settings", {}, [this] { show_initial_settings(); });
    action(simulation_menu, "expression_settings", {}, [this] { show_expression_settings(); });
    action(simulation_menu, "step_settings", {}, [this] { show_step_settings(); });
    action(simulation_menu, "experiments", {}, [this] { show_experiments(); });
    auto *examples = menuBar()->addMenu(text("examples"));
    build_examples_menu(examples);
    auto *toolbar = addToolBar("PowerDriveSim");
    toolbar->setObjectName("controls");
    toolbar->setMovable(false);
    auto *brand = new QLabel("∿  PowerDriveSim");
    brand->setStyleSheet("font-size:17px;font-weight:600;color:white;padding-right:20px;");
    toolbar->addWidget(brand);
    auto *duration_label = new QLabel(text("duration"));
    duration_label->setStyleSheet("padding:0 8px 0 4px;");
    toolbar->addWidget(duration_label);
    stop_ = new QLineEdit;
    normalize_decimal_point(stop_);
    stop_->setObjectName("sim_stop");
    stop_->setFixedWidth(90);
    toolbar->addWidget(stop_);
    auto *step_label = new QLabel(text("step_short"));
    step_label->setStyleSheet("padding:0 8px 0 10px;");
    toolbar->addWidget(step_label);
    step_ = new QLineEdit;
    normalize_decimal_point(step_);
    step_->setObjectName("sim_step");
    step_->setFixedWidth(90);
    toolbar->addWidget(step_);
    method_ = new QComboBox;
    method_->setObjectName("sim_method");
    method_->addItems({"Backward Euler", "Trapezoidal"});
    method_->setToolTip(text("method"));
    method_->setFixedWidth(150);
    toolbar->addWidget(method_);
    run_ = action(nullptr, "run", QKeySequence("Ctrl+R"), [this] { start_simulation(); });
    auto *run_button = new QToolButton;
    run_button->setObjectName("run_button");
    run_button->setDefaultAction(run_);
    run_->setIcon(ui_icon(UiIcon::play, true));
    run_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    run_button->setIconSize({16, 16});
    run_button->setFixedSize(132, 36);
    toolbar->addWidget(run_button);
    stop_action_ = action(nullptr, "stop", QKeySequence("Escape"), [this] { stop_simulation(); });
    simulation_menu->insertAction(continue_action, run_);
    simulation_menu->insertAction(continue_action, stop_action_);
    simulation_menu->insertSeparator(continue_action);
    auto *stop_button = new QToolButton;
    stop_button->setObjectName("stop_button");
    stop_button->setDefaultAction(stop_action_);
    stop_action_->setIcon(ui_icon(UiIcon::stop, true));
    stop_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    stop_button->setIconSize({16, 16});
    stop_button->setFixedSize(132, 36);
    toolbar->addWidget(stop_button);
    for (auto [command, icon] : {std::pair{step_action, UiIcon::step}}) {
        command->setIcon(ui_icon(icon));
        auto *button = new QToolButton;
        button->setDefaultAction(command);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setIconSize({20, 20});
        button->setFixedSize(36, 36);
        button->setStyleSheet("QToolButton{padding:4px;}");
        toolbar->addWidget(button);
    }
    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);
    auto *center = new QWidget;
    auto *layout = new QVBoxLayout(center);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *canvas_tools = new QWidget;
    canvas_tools->setStyleSheet("background:palette(base);border-bottom:1px solid palette(mid);");
    auto *tools_layout = new QHBoxLayout(canvas_tools);
    tools_layout->setContentsMargins(16, 8, 16, 8);
    auto *title = new QWidget;
    breadcrumbs_ = title;
    title->setObjectName("hierarchy_breadcrumbs");
    title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *breadcrumb_layout = new QHBoxLayout(title);
    breadcrumb_layout->setContentsMargins(16, 5, 16, 5);
    breadcrumb_layout->setSpacing(4);
    layout->addWidget(title);
    tools_layout->addStretch();
    for (auto *a : {wire_action, undo_, redo_, fit_action}) {
        auto *button = new QToolButton;
        button->setDefaultAction(a);
        tools_layout->addWidget(button);
    }
    layout->addWidget(canvas_tools);
    canvas_ = new Canvas;
    // A plain letter shortcut must only be active on the schematic canvas;
    // otherwise typing a 'g' in a name or script would open the dialog.
    removeAction(grid_action);
    canvas_->addAction(grid_action);
    grid_action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    layout->addWidget(canvas_, 1);
    banner_ = new QLabel(text("hint"));
    banner_->setStyleSheet(
        "padding:9px 16px;color:palette(placeholder-text);background:palette(window);border-top:1px solid "
        "palette(mid);");
    banner_->setWordWrap(true);
    layout->addWidget(banner_);
    simulation_progress_ = new QProgressBar;
    simulation_progress_->setObjectName("simulation_progress");
    simulation_progress_->setTextVisible(true);
    simulation_progress_->setMinimumHeight(18);
    simulation_progress_->setMaximumHeight(18);
    simulation_progress_->setVisible(false);
    layout->addWidget(simulation_progress_);
    setCentralWidget(center);
    auto dock = [&](const char *key, QWidget *widget, Qt::DockWidgetArea area) {
        auto *d = new QDockWidget(text(key), this);
        d->setObjectName(key);
        d->setFeatures(QDockWidget::NoDockWidgetFeatures);
        d->setWidget(widget);
        addDockWidget(area, d);
        return d;
    };
    auto *left_tabs = new QTabWidget;
    left_tabs->setObjectName("workspace_tabs");
    auto *lib = new QWidget;
    auto *ll = new QVBoxLayout(lib);
    ll->setContentsMargins(10, 12, 10, 8);
    ll->setSpacing(12);
    auto *search = new QLineEdit;
    search->setPlaceholderText(text("search"));
    ll->addWidget(search);
    library_ = new QTreeWidget;
    library_->setObjectName("library");
    library_->setColumnCount(1);
    library_->setHeaderHidden(true);
    library_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    library_->setIconSize({28, 28});
    library_->setIndentation(16);
    library_->setRootIsDecorated(true);
    library_->setUniformRowHeights(true);
    ll->addWidget(library_);
    load_component_specs();
    build_component_palette(search);
    left_tabs->addTab(lib, text("library"));
    objects_ = new QListWidget;
    connect(objects_, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *i) { select_object(i->data(Qt::UserRole).toString().toStdString()); });
    left_tabs->addTab(objects_, text("objects"));
    hierarchy_ = new QTreeWidget;
    hierarchy_->setHeaderHidden(true);
    hierarchy_->setObjectName("hierarchy_tree");
    left_tabs->addTab(hierarchy_, text("hierarchy"));
    connect(hierarchy_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        auto path = item->data(0, Qt::UserRole).toStringList();
        std::vector<std::string> steps;
        for (const auto &step : path)
            steps.push_back(step.toStdString());
        navigate_hierarchy(steps);
    });
    auto *left = dock("workspace", left_tabs, Qt::LeftDockWidgetArea);
    left->setMinimumWidth(300);
    left->setMaximumWidth(380);
    right_tabs_ = new QTabWidget;
    right_tabs_->setObjectName("right_workspace_tabs");
    auto *variables_page = new QWidget;
    auto *variables_layout = new QVBoxLayout(variables_page);
    variables_layout->setContentsMargins(8, 8, 8, 8);
    workspace_variables_ = new QTableWidget;
    workspace_variables_->setObjectName("workspace_variables");
    workspace_variables_->setColumnCount(2);
    workspace_variables_->setHorizontalHeaderLabels({text("name"), text("value")});
    workspace_variables_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    workspace_variables_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    workspace_variables_->verticalHeader()->hide();
    workspace_variables_->setEditTriggers(QAbstractItemView::DoubleClicked |
                                           QAbstractItemView::SelectedClicked |
                                           QAbstractItemView::EditKeyPressed);
    workspace_variables_->setSelectionBehavior(QAbstractItemView::SelectRows);
    workspace_variables_->setAlternatingRowColors(true);
    variables_layout->addWidget(workspace_variables_);
    connect(workspace_variables_, &QTableWidget::cellChanged, this,
            [this](int row, int column) { edit_workspace_variable(row, column); });
    right_tabs_->addTab(variables_page, text("variables"));
    inspector_stack_ = new QStackedWidget;
    inspector_stack_->setObjectName("inspector_stack");
    inspector_page_ = create_inspector_page();
    inspector_stack_->addWidget(inspector_page_);
    for (auto *field : {stop_, step_})
        connect(field, &QLineEdit::editingFinished, this, [this] { commit_profile(); });
    connect(method_, &QComboBox::activated, this, [this] { commit_profile(); });
    auto *right = dock("inspector", right_tabs_, Qt::RightDockWidgetArea);
    right->setWindowTitle(text("workspace"));
    right->setMinimumWidth(280);
    right->setMaximumWidth(400);
    bottom_ = new QTabWidget;
    bottom_->setObjectName("results_tabs");
    auto *diagnostics = new QWidget;
    auto *dl = new QVBoxLayout(diagnostics);
    auto *diagnostic_hint = new QLabel(text("diagnostics_hint"));
    diagnostic_hint->setObjectName("diagnostic_status");
    diagnostic_hint->setStyleSheet("padding:5px 12px;color:palette(placeholder-text);");
    dl->addWidget(diagnostic_hint);
    errors_ = new QListWidget;
    errors_->setObjectName("diagnostics_list");
    errors_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    errors_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    errors_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(errors_, &QWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        if (auto *item = errors_->itemAt(point); item && !item->isSelected())
            errors_->setCurrentItem(item);
        QMenu menu(errors_);
        auto *copy = menu.addAction(text("copy"));
        copy->setEnabled(!errors_->selectedItems().empty());
        connect(copy, &QAction::triggered, this, [this] { copy_diagnostics(); });
        menu.exec(errors_->viewport()->mapToGlobal(point));
    });
    dl->addWidget(errors_, 1);
    bottom_->addTab(diagnostics, text("diagnostics"));
    connect(errors_, &QListWidget::itemClicked, this, [this](QListWidgetItem *i) {
        const auto object = i->data(Qt::UserRole).toString().toStdString();
        std::vector<std::string> path;
        for (const auto &step : i->data(Qt::UserRole + 1).toStringList())
            path.push_back(step.toStdString());
        navigate_hierarchy(path);
        select_object(object);
        errors_->setFocus();
    });
    scope_page_ = new QWidget;
    scope_layout_ = new QVBoxLayout(scope_page_);
    scope_layout_->setContentsMargins(6, 4, 6, 4);
    scope_layout_->setSpacing(3);
    auto *scope_header = new QHBoxLayout;
    scope_enable_ = new QCheckBox(text("scope_record"));
    scope_enable_->setObjectName("scope_enable");
    scope_header->addWidget(scope_enable_);
    scope_header->addStretch();
    scope_fit_ = new QPushButton(text("fit"));
    scope_fit_->setObjectName("scope_fit");
    scope_fit_->setEnabled(false);
    scope_fit_->hide();
    scope_header->addWidget(scope_fit_);
    connect(scope_fit_, &QPushButton::clicked, this, [this] {
        if (scope_)
            scope_->fit();
    });
    scope_export_ = new QPushButton(ui_icon(UiIcon::export_data), QString());
    scope_export_->setObjectName("scope_export");
    scope_export_->setToolTip(text("export_selected"));
    scope_export_->setAccessibleName(text("export_selected"));
    scope_export_->setIconSize({22, 22});
    scope_export_->setFixedSize(32, 32);
    scope_export_->setStyleSheet("padding:3px;");
    scope_export_->setEnabled(false);
    scope_header->addWidget(scope_export_);
    scope_layout_->addLayout(scope_header);
    scope_hint_ = new QLabel(text("scope_disabled"));
    scope_hint_->setWordWrap(true);
    scope_hint_->setAlignment(Qt::AlignCenter);
    scope_hint_->setStyleSheet("color:palette(placeholder-text);padding:24px;font-size:13px;");
    scope_hint_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    scope_layout_->addWidget(scope_hint_, 1);
    bottom_->addTab(scope_page_, text("scope_tab"));
    connect(scope_enable_, &QCheckBox::toggled, this, [this](bool checked) {
        if (!rebuilding_ && document_)
            set_scope_enabled(checked);
    });
    connect(scope_export_, &QPushButton::clicked, this, [this] { export_csv(project().scope_channels); });
    auto *bottom_dock = dock("results", bottom_, Qt::BottomDockWidgetArea);
    bottom_dock->setTitleBarWidget(new QWidget);
    bottom_dock->setMinimumHeight(0);
    bottom_->setMinimumHeight(0);
    scope_page_->setMinimumHeight(0);
    resizeDocks({bottom_dock}, {180}, Qt::Vertical);
    connect(bottom_, &QTabWidget::currentChanged, this, [bottom_dock](int tab) {
        (void)tab;
        bottom_dock->setMinimumHeight(0);
    });
    auto *expand_scope = new QToolButton;
    expand_scope->setIcon(ui_icon(UiIcon::undock));
    expand_scope->setIconSize({22, 22});
    expand_scope->setFixedSize(32, 32);
    expand_scope->setStyleSheet("padding:3px;");
    expand_scope->setToolTip(text("undock_scope"));
    expand_scope->setAccessibleName(text("undock_scope"));
    expand_scope->setObjectName("undock_scope");
    scope_header->addWidget(expand_scope);
    bottom_dock->setFeatures(QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetMovable);
    connect(expand_scope, &QToolButton::clicked, this, [bottom_dock, expand_scope] {
        bool floating = !bottom_dock->isFloating();
        bottom_dock->setFloating(floating);
        if (floating)
            bottom_dock->resize(1150, 720);
        expand_scope->setIcon(ui_icon(floating ? UiIcon::dock : UiIcon::undock));
        expand_scope->setToolTip(text(floating ? "dock_scope" : "undock_scope"));
        expand_scope->setAccessibleName(text(floating ? "dock_scope" : "undock_scope"));
    });
    canvas_->place = [this](QPointF point) { place_at(point); };
    canvas_->cancel_placement = [this] { cancel_placement(); };
    canvas_->quick_insert = [this](QPointF point) { quick_insert(point); };
    canvas_->add_junction = [this](QPointF point) { add_node(false, point); };
    canvas_->connect_wire = [this](WireAnchor from, WireAnchor to, std::vector<Point> bends,
                                   std::string replace) {
        bends = clean_route_bends(QPointF(from.point.x, from.point.y), QPointF(to.point.x, to.point.y), bends);
        connect_gesture(from, to, std::move(bends), replace);
    };
    canvas_->route_preview = [this](Endpoint a, std::optional<Endpoint> b, QPointF start, QPointF end) {
        return preview_route(a, b, start, end);
    };
    canvas_->wire_endpoint = [this](const std::string &id, bool from) {
        if (auto view = hidden_current_wire_view(project(), id); view && id == view->primary)
            return from ? view->from : view->to;
        for (const auto &w : project().wires)
            if (w.id == id)
                return from ? w.from : w.to;
        return Endpoint{};
    };
    canvas_->select_conductor = [this](const std::string &id) {
        auto source = std::find_if(project().wires.begin(), project().wires.end(),
                                   [&](const Wire &wire) { return wire.id == id; });
        if (source == project().wires.end())
            return;
        const auto graph = resolve_connections(project());
        auto network = graph.nets.find(endpoint_key(source->from));
        if (network == graph.nets.end())
            network = graph.nets.find(endpoint_key(source->to));
        rebuilding_ = true;
        canvas_->scene()->clearSelection();
        bool selected = false;
        for (const auto &wire : project().wires) {
            auto candidate = graph.nets.find(endpoint_key(wire.from));
            if (candidate == graph.nets.end())
                candidate = graph.nets.find(endpoint_key(wire.to));
            if (network != graph.nets.end() && candidate != graph.nets.end() &&
                candidate->second == network->second) {
                auto *item = wires_.at(wire.id);
                item->setData(wire_segment_role, 0);
                item->setSelected(true);
                selected = true;
            }
        }
        if (!selected) {
            auto *item = wires_.at(id);
            item->setData(wire_segment_role, 0);
            item->setSelected(true);
        }
        selected_ = id;
        rebuilding_ = false;
        fill_inspector();
        update_wires();
        update_command_state();
        canvas_->viewport()->update();
    };
    canvas_->edit_route = [this](std::string id, std::vector<Point> bends) {
        if (running())
            return;
        try {
            for (auto &point : bends) {
                auto snapped = canvas_->snap_point({point.x, point.y});
                point = {snapped.x(), snapped.y()};
            }
            Endpoint from, to;
            if (auto view = hidden_current_wire_view(project(), id); view && id == view->primary) {
                from = view->from;
                to = view->to;
            }
            for (const auto &wire : project().wires)
                if (wire.id == id && from.object.empty()) {
                    from = wire.from;
                    to = wire.to;
                    break;
                }
            struct JunctionMove {
                std::string id;
                QPointF point;
                std::map<std::string, std::vector<Point>> routes;
            };
            std::optional<JunctionMove> junction_move;
            auto generated_node = [](const Node &node) {
                if (node.ground)
                    return false;
                if (node.name.empty())
                    return true;
                return node.name[0] == 'N' &&
                       std::all_of(node.name.begin() + 1, node.name.end(),
                                   [](unsigned char c) { return std::isdigit(c); });
            };
            auto consider_junction = [&](const Endpoint &endpoint, bool at_start) {
                if (bends.empty() || endpoint.port != "node")
                    return;
                const auto node = std::find_if(project().nodes.begin(), project().nodes.end(),
                                               [&](const Node &candidate) { return candidate.id == endpoint.object; });
                if (node == project().nodes.end() || !generated_node(*node))
                    return;
                struct TrunkEnd {
                    std::string wire;
                    QPointF adjacent;
                    bool node_is_from = false;
                };
                std::vector<TrunkEnd> trunk;
                for (const auto &wire : project().wires) {
                    if (wire.id == id || (wire.from.object != node->id && wire.to.object != node->id))
                        continue;
                    const auto graphics = wires_.find(wire.id);
                    if (graphics == wires_.end() || !graphics->second->isVisible())
                        continue;
                    const auto path = static_cast<QGraphicsPathItem *>(graphics->second)->path();
                    if (path.elementCount() < 2)
                        continue;
                    const bool node_is_from = wire.from.object == node->id;
                    const auto adjacent_element = path.elementAt(node_is_from ? 1 : path.elementCount() - 2);
                    trunk.push_back({wire.id, {adjacent_element.x, adjacent_element.y}, node_is_from});
                }
                if (trunk.size() < 2)
                    return;
                const auto &bend = at_start ? bends.front() : bends.back();
                const QPointF candidate = canvas_->snap_point({bend.x, bend.y});
                const QPointF old_node = canvas_->snap_point(port_position(endpoint));
                auto between = [](double value, double a, double b) {
                    return value >= std::min(a, b) - 1e-6 && value <= std::max(a, b) + 1e-6;
                };
                for (size_t i = 0; i < trunk.size(); ++i)
                    for (size_t j = i + 1; j < trunk.size(); ++j) {
                        const bool horizontal = std::abs(trunk[i].adjacent.y() - old_node.y()) < 1e-6 &&
                                                std::abs(trunk[j].adjacent.y() - old_node.y()) < 1e-6 &&
                                                std::abs(candidate.y() - old_node.y()) < 1e-6 &&
                                                between(candidate.x(), trunk[i].adjacent.x(),
                                                        trunk[j].adjacent.x());
                        const bool vertical = std::abs(trunk[i].adjacent.x() - old_node.x()) < 1e-6 &&
                                              std::abs(trunk[j].adjacent.x() - old_node.x()) < 1e-6 &&
                                              std::abs(candidate.x() - old_node.x()) < 1e-6 &&
                                              between(candidate.y(), trunk[i].adjacent.y(),
                                                      trunk[j].adjacent.y());
                        if (horizontal || vertical) {
                            JunctionMove move{node->id, candidate, {}};
                            for (const auto &end : trunk) {
                                const auto path = static_cast<QGraphicsPathItem *>(wires_.at(end.wire))->path();
                                QPainterPath adjusted;
                                for (int index = 0; index < path.elementCount(); ++index) {
                                    const auto element = path.elementAt(index);
                                    QPointF point(element.x, element.y);
                                    if ((end.node_is_from && index == 0) ||
                                        (!end.node_is_from && index == path.elementCount() - 1))
                                        point = candidate;
                                    if (index == 0)
                                        adjusted.moveTo(point);
                                    else
                                        adjusted.lineTo(point);
                                }
                                move.routes[end.wire] = route_bends(adjusted);
                            }
                            junction_move = std::move(move);
                            return;
                        }
                    }
            };
            consider_junction(from, true);
            consider_junction(to, false);
            QPointF route_from = port_position(from), route_to = port_position(to);
            if (junction_move) {
                if (from.object == junction_move->id)
                    route_from = junction_move->point;
                if (to.object == junction_move->id)
                    route_to = junction_move->point;
            }
            bends = clean_route_bends(route_from, route_to, bends);
            if (junction_move)
                junction_move->routes[id] = bends;
            document_->apply("Edit route", [&](Project &p) {
                for (auto &w : p.wires) {
                    if (junction_move) {
                        const auto route = junction_move->routes.find(w.id);
                        if (route != junction_move->routes.end())
                            w.bends = route->second;
                    } else if (w.id == id) {
                        w.bends = bends;
                    }
                }
                if (junction_move)
                    for (auto &node : p.nodes)
                        if (node.id == junction_move->id) {
                            node.x = junction_move->point.x();
                            node.y = junction_move->point.y();
                        }
            });
            refresh_canvas(false, false);
        } catch (const std::exception &e) {
            show_error(e);
            refresh_canvas(false, false);
        }
    };
    canvas_->compatible = [this](const Endpoint &a, const Endpoint &b) {
        try {
            validate_wire(project(), Wire{"", a, b, {}});
            return true;
        } catch (...) {
            return false;
        }
    };
    canvas_->context_menu = [this](std::string id, QPoint point) { show_context(id, point); };
    load_shortcuts();
    canvas_->movement = [this] {
        for (auto *item : canvas_->scene()->selectedItems())
            if (item->data(1).toString() == "atom" &&
                (item->data(10).toBool() || item->data(12).toBool() || item->data(17).toBool())) {
                auto *atom = static_cast<Atom *>(item);
                atom->snap_ports_to_grid(canvas_->grid_size());
                atom->separate_overlapping_ports(canvas_->grid_size());
            }
        update_labels();
        update_wires();
    };
    canvas_->released = [this] { commit_positions(); };
    canvas_->open_object = [this](std::string id) {
        if (std::any_of(project().code_blocks.begin(), project().code_blocks.end(),
                        [&](const CodeBlock &block) { return block.id == id; })) {
            edit_code_block(id);
            return;
        }
        if (std::any_of(project().instances.begin(), project().instances.end(),
                        [&](const auto &i) { return i.id == id; })) {
            open_subcircuit(id);
            return;
        }
        if (std::any_of(project().plots.begin(), project().plots.end(),
                        [&](const PlotBlock &p) { return p.id == id; }))
            open_plot(id);
        else {
            select_object(id);
            auto *field = value_ && value_->isVisible() ? value_ : name_;
            if (field && field->isVisible()) {
                field->setFocus();
                field->selectAll();
            }
        }
    };
    canvas_->edit_text = [this](QPoint point) { return edit_text_at(point); };
    connect(canvas_->scene(), &QGraphicsScene::selectionChanged, this, [this] {
        if (rebuilding_)
            return;
        auto items = canvas_->scene()->selectedItems();
        selected_ = items.empty() ? "" : items.front()->data(0).toString().toStdString();
        fill_inspector();
        update_wires();
        update_command_state();
    });
    qApp->installEventFilter(this);
    connect(qApp, &QApplication::focusChanged, this,
            [this](QWidget *, QWidget *) { update_command_state(); });
}
void EditorWindow::set_project(Project p) {
    // Old versions serialized two zero-degree junctions when a complete wire
    // segment was deleted. They have no electrical meaning and must not return
    // as detached squares when such a project is opened again.
    prune_orphan_nodes(p);
    cancel_inline_edit();
    rebuilding_ = true;
    for (auto &[id, window] : plot_windows_) {
        (void)id;
        if (window)
            delete window;
    }
    plot_windows_.clear();
    plot_views_.clear();
    canvas_->cancel_gesture();
    labels_.clear();
    atoms_.clear();
    wires_.clear();
    base_wire_routes_.clear();
    canvas_->scene()->clear();
    scene_project_.reset();
    route_positions_.clear();
    obstacle_cache_.clear();
    topology_dirty_ = true;
    drafts_.clear();
    inspector_id_.clear();
    clear_result();
    continuation_.reset();
    document_ = std::make_unique<Document>(std::move(p));
    auto *old_scope_content = scope_content_;
    scope_content_ = nullptr;
    scope_ = nullptr;
    channels_ = nullptr;
    delete old_scope_content;
    stop_->setModified(false);
    step_->setModified(false);
    selected_.clear();

    path_.clear();
    example_origin_.clear();
    saved_state_ = serialized(root_project());
    refresh();
    canvas_->resetTransform();
    canvas_->centerOn(200, 120);
    if (!project().components.empty())
        canvas_->fitInView(canvas_->scene()->itemsBoundingRect().adjusted(-80, -80, 80, 80),
                           Qt::KeepAspectRatio);
}
void EditorWindow::new_file() {
    Project p;
    p.id = new_uuid();
    p.name = text("untitled").toStdString();
    p.wired = true;
    set_project(std::move(p));
    banner_->setText(text("hint"));
}
bool EditorWindow::open_project(const QString &path) {
    try {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error(file.errorString().toStdString());
        std::istringstream in(file.readAll().toStdString());
        auto opened = read_project(in);
        const bool example = bundled_example(path);
        if (example) {
            const auto key = "example_" + QFileInfo(path).completeBaseName();
            const auto title = text(key.toUtf8().constData());
            if (title != key) opened.name = title.toStdString();
        }
        set_project(std::move(opened));
        if (example) example_origin_ = path;
        else {
            path_ = path;
            QSettings settings(recovery_dir_ + "/ui.ini", QSettings::IniFormat);
            settings.setValue("last_project_dir", QFileInfo(path).absolutePath());
        }
        update_title();
        banner_->setText(text("hint"));
        return true;
    } catch (const std::exception &e) {
        show_error(e);
        return false;
    }
}
bool EditorWindow::save_project(const QString &path) {
    if (bundled_example(path)) {
        banner_->setText(text("example_save_copy"));
        return false;
    }
    if (!commit_inline_edit())
        return false;
    try {
        QSaveFile file(path);
        auto bytes = serialized(root_project());
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(bytes.data(), static_cast<qint64>(bytes.size())) !=
                static_cast<qint64>(bytes.size()) ||
            !file.commit())
            throw std::runtime_error(file.errorString().toStdString());
        path_ = path;
        QSettings settings(recovery_dir_ + "/ui.ini", QSettings::IniFormat);
        settings.setValue("last_project_dir", QFileInfo(path).absolutePath());
        saved_state_ = bytes;
        update_title();
        banner_->setText(text("saved"));
        return true;
    } catch (const std::exception &e) {
        show_error(e);
        return false;
    }
}
QString EditorWindow::recovery_path() const {
    return recovery_dir_ + "/recovery.pds.autosave";
}
bool EditorWindow::autosave() {
    if (!document_ || serialized(root_project()) == saved_state_)
        return false;
    QDir().mkpath(recovery_dir_);
    QSaveFile file(recovery_path());
    auto bytes = serialized(root_project());
    return file.open(QIODevice::WriteOnly) &&
           file.write(bytes.data(), static_cast<qint64>(bytes.size())) == static_cast<qint64>(bytes.size()) &&
           file.commit();
}
void EditorWindow::update_autosave_timer() {
    if (!autosave_timer_)
        return;
    QSettings settings(recovery_dir_ + "/ui.ini", QSettings::IniFormat);
    const int seconds = settings.value("autosave_interval_seconds", 15).toInt();
    autosave_timer_->stop();
    if (seconds > 0)
        autosave_timer_->start(std::clamp(seconds, 1, 3600) * 1000);
}
void EditorWindow::configure_autosave() {
    QSettings settings(recovery_dir_ + "/ui.ini", QSettings::IniFormat);
    bool ok = false;
    const int current = settings.value("autosave_interval_seconds", 15).toInt();
    const int seconds = QInputDialog::getInt(this, text("autosave_settings"), text("autosave_interval"),
                                             current, 0, 3600, 5, &ok);
    if (!ok)
        return;
    settings.setValue("autosave_interval_seconds", seconds);
    settings.sync();
    update_autosave_timer();
    banner_->setText(seconds > 0 ? text("autosave_updated").arg(seconds) : text("autosave_disabled"));
}
void EditorWindow::configure_grid() {
    QSettings settings(recovery_dir_ + "/ui.ini", QSettings::IniFormat);
    QDialog dialog(this);
    dialog.setObjectName("grid_settings_dialog");
    dialog.setWindowTitle(text("grid_settings"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *style = new QComboBox;
    style->setObjectName("grid_style");
    style->addItems({text("grid_dots"), text("grid_lines"), text("grid_hidden")});
    style->setCurrentIndex(int(canvas_->grid_style()));
    auto *size = new QSpinBox;
    size->setObjectName("grid_size");
    size->setRange(5, 100);
    size->setSingleStep(5);
    size->setSuffix(" px");
    size->setValue(int(std::lround(canvas_->grid_size())));
    auto make_width = [&](const char *name, double value) {
        auto *spin = new QDoubleSpinBox;
        spin->setObjectName(name);
        spin->setRange(0.5, 8.0);
        spin->setSingleStep(0.25);
        spin->setDecimals(2);
        spin->setSuffix(" px");
        spin->setValue(value);
        return spin;
    };
    auto *line_width = make_width("grid_line_width", canvas_->grid_line_width());
    auto *dot_size = make_width("grid_dot_size", canvas_->grid_dot_size());
    form->addRow(text("grid_style"), style);
    form->addRow(text("grid_size"), size);
    form->addRow(text("grid_line_width"), line_width);
    form->addRow(text("grid_dot_size"), dot_size);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    canvas_->set_grid_style(static_cast<Canvas::GridStyle>(style->currentIndex()));
    canvas_->set_grid_size(size->value());
    canvas_->set_grid_line_width(line_width->value());
    canvas_->set_grid_dot_size(dot_size->value());
    settings.setValue("grid_style", style->currentIndex());
    settings.setValue("grid_size", size->value());
    settings.setValue("grid_line_width", line_width->value());
    settings.setValue("grid_dot_size", dot_size->value());
    settings.sync();
    refresh_canvas(false, false);
    banner_->setText(text("grid_updated").arg(size->value()));
}
bool EditorWindow::recover(const QString &path) {
    if (!open_project(path))
        return false;
    path_.clear();
    saved_state_.clear();
    update_title();
    banner_->setText(text("recovered"));
    return true;
}
void EditorWindow::refresh(bool invalidate, bool navigation_only) {
    rebuilding_ = true;
    if (scene_path_ != hierarchy_path()) {
        canvas_->cancel_gesture();
        cancel_inline_edit();
        atoms_.clear();
        wires_.clear();
        labels_.clear();
        canvas_->scene()->clear();
        base_wire_routes_.clear();
        route_positions_.clear();
        obstacle_cache_.clear();
        scene_project_.reset();
        selected_.clear();
        drafts_.clear();
        inspector_id_.clear();
        scene_path_ = hierarchy_path();
        hierarchy_edit_enabled_ = false;
        topology_dirty_ = true;
    }
    if(!navigation_only)update_instance_specs();
    const bool model_changed = !scene_project_ || !same_simulation(*scene_project_, project());
    auto labels = [](const Project &p) {
        std::map<std::string, std::string> result;
        auto collect = [&](const auto &objects) {
            for (const auto &o : objects)
                result[o.id] = o.name;
        };
        collect(p.nodes);
        collect(p.components);
        collect(p.tags);
        collect(p.patterns);
        collect(p.plots);
        collect(p.instances);
        return result;
    };
    const bool labels_changed = !scene_project_ || labels(*scene_project_) != labels(project());
    const bool recording_changed = !scene_project_ ||
                                   scene_project_->scope_enabled != project().scope_enabled ||
                                   scene_project_->scope_channels != project().scope_channels;
    topology_dirty_ |= model_changed;
    if (invalidate && result_ && result_project_ && !same_simulation(*result_project_, root_project())) {
        auto previous = *result_project_;
        previous.profile.stop = root_project().profile.stop;
        if (!same_simulation(previous, root_project())) continuation_.reset();
        clear_result();
        banner_->setText(text("result_outdated"));
    }
    rebuild_scene();
    objects_->clear();
    auto item = [&](const std::string &id, const std::string &name) {
        auto *i = new QListWidgetItem(q(name), objects_);
        i->setData(Qt::UserRole, q(id));
    };
    for (const auto &c : project().components)
        if (!is_hidden_current_probe(project(), c.id))
            item(c.id, c.name);
    for (const auto &n : project().nodes)
        item(n.id, n.name);
    for (const auto &t : project().tags)
        item(t.id, t.name);
    for (const auto &g : project().patterns)
        item(g.id, g.name);
    for (const auto &g : project().plots)
        item(g.id, g.name);
    for (const auto &block : project().code_blocks)
        item(block.id, block.name);
    for (const auto &i : project().instances)
        item(i.id, i.name);
    if (!stop_->isModified())
        stop_->setText(QString::number(project().profile.stop, 'g', 12));
    if (!step_->isModified())
        step_->setText(QString::number(project().profile.step, 'g', 12));
    method_->setCurrentIndex(project().profile.method == Method::trapezoidal ? 1 : 0);
    undo_->setEnabled(document_->can_undo() && !running());
    redo_->setEnabled(document_->can_redo() && !running());
    if(!navigation_only)sync_scope();
    if (!navigation_only && (model_changed || labels_changed || recording_changed))
        refresh_channel_catalog();
    if (labels_changed && result_) {
        try {
            auto catalog = available_channels(compile(root_project()));
            for (auto &channel : result_->channels)
                for (const auto &fresh : catalog)
                    if (channel.object == fresh.object) {
                        channel.name = fresh.name;
                        break;
                    }
        } catch (const Diagnostic &) {
        }
    }
    if (scope_)
        scope_->set_result(result_ ? &*result_ : nullptr, result_indices(project().scope_channels),
                           project());
    if(!navigation_only) {
        update_graphs();
        update_workspace_variables();
    }
    scene_project_ = project();
    rebuilding_ = false;
    fill_inspector();
    refresh_hierarchy();
    update_command_state();
    update_title();
}
void EditorWindow::refresh_canvas(bool invalidate, bool topology) {
    if (scene_path_ != hierarchy_path()) {
        refresh(invalidate);
        return;
    }
    topology_dirty_ = topology_dirty_ || topology;
    if (invalidate && result_ && result_project_ && !same_simulation(*result_project_, root_project())) {
        auto previous = *result_project_;
        previous.profile.stop = root_project().profile.stop;
        if (!same_simulation(previous, root_project()))
            continuation_.reset();
        result_project_.reset();
        banner_->setText(text("result_outdated"));
    }
    rebuilding_ = true;
    rebuild_scene();
    scene_project_ = project();
    rebuilding_ = false;
    fill_inspector();
    update_command_state();
    update_title();
}
void EditorWindow::rebuild_scene() {
    std::set<std::string> present;
    auto atom = [&](const std::string &id, const std::string &name, const QString &symbol, int type) {
        present.insert(id);
        Atom *a = nullptr;
        if (atoms_.count(id))
            a = static_cast<Atom *>(atoms_.at(id));
        else {
            a = new Atom(id, q(name), symbol, type);
            atoms_[id] = a;
            canvas_->scene()->addItem(a);
        }
        a->separate_labels = true;
        a->name = q(name);
        a->symbol = symbol;
        a->type = type;
        a->code_icon.clear();
        if(auto appearance=std::find_if(project().object_icons.begin(),project().object_icons.end(),
                [&](const ObjectIcon &candidate){return candidate.object==id;});appearance!=project().object_icons.end())
            a->code_icon=appearance->primitives;
        a->setData(10, type == 4);
        a->setData(12, type == 3);
        a->setData(17, type == 2);
        // Resize handles follow the visible body, not labels and port leads
        // included in boundingRect() for painting and hit testing.
        a->setData(15, type == 2 ? QVariant(QRectF(-38, -22, 76, 44)) : QVariant{});
        return a;
    };
    auto ports = [&](Atom *a, const std::vector<std::pair<QString, QPointF>> &list, const QColor &color) {
        if (a->childItems().size() != static_cast<qsizetype>(list.size())) {
            for (auto *child : a->childItems())
                delete child;
            for (auto &[name, point] : list)
                a->port(name, point, color);
        } else
            for (auto *child : a->childItems())
                for (const auto &[name, point] : list)
                    if (child->data(2).toString() == name) {
                        child->setPos(point);
                        child->setData(11, color.name(QColor::HexRgb));
                    }
    };
    for (const auto &c : project().components) {
        auto *a = atom(c.id, c.name, q(kind_name(c.kind)), 0);
        const bool hidden_probe = is_hidden_current_probe(project(), c.id);
        a->setVisible(!hidden_probe);
        a->setFlag(QGraphicsItem::ItemIsSelectable, !hidden_probe);
        if (hidden_probe)
            a->setSelected(false);
        a->value = component_label(c);
        std::vector<std::pair<QString, QPointF>> list = hidden_probe
            ? std::vector<std::pair<QString, QPointF>>{{"p", {0, 0}}, {"n", {0, 0}}}
            : std::vector<std::pair<QString, QPointF>>{{"p", {-60, 0}}, {"n", {60, 0}}};
        if (gate_controlled(c.kind))
            list.push_back({"gate", {0, -40}});
        if (c.kind == Kind::voltage_probe || c.kind == Kind::current_probe)
            list.push_back({"out", {0, -40}});
        ports(a, list, QColor("#146cca"));
        if (gate_controlled(c.kind))
            for (auto *child : a->childItems())
                if (child->data(2).toString() == "gate")
                    child->setData(11, QColor("#17866d").name(QColor::HexRgb));
    }
    for (const auto &n : project().nodes) {
        auto *a = atom(n.id, n.ground ? n.name : std::string(), {}, 1);
        a->ground = n.ground;
        int degree = 0;
        for (const auto &w : project().wires)
            degree += w.from.object == n.id || w.to.object == n.id;
        a->setData(4, degree);
        a->setToolTip(text(n.ground ? "ground" : degree >= 3 ? "junction_branch" : "junction_pass"));
        ports(a, {{"node", {0, 0}}}, QColor("#146cca"));
    }
    for (const auto &t : project().tags) {
        auto *a = atom(t.id, t.name, tag_symbol(t.domain), 5);
        a->value.clear();
        ports(a, {{"io", {0, 0}}}, domain_color(t.domain));
    }
    for (const auto &g : project().patterns) {
        auto *a = atom(g.id, g.name, g.script ? QString("Code") : g.pwm ? QString("PWM") : QString(), 2);
        if(a->input_count!=g.outputs)a->prepareGeometryChangeForInputs(g.outputs);
        const double gate_h=std::max(22.0,(std::max(1u,g.outputs)-1)*10.0+14.0);
        a->setData(15,QRectF(-38,-gate_h,76,2*gate_h));
        a->value = g.script ? QString()
                  : g.pwm   ? QString::number(g.frequency) + " Hz · " + QString::number(g.duty * 100) + " %"
                            : QString();
        std::vector<std::pair<QString,QPointF>> list;
        for(unsigned index=0;index<g.outputs;++index) {
            const double y=(double(index)-double(g.outputs-1)/2.0)*20.0;
            const QString name=index==0?QString("out"):QString("out")+QString::number(index);
            QPointF point{45,y};
            if(auto stored=std::find_if(g.pin_positions.begin(),g.pin_positions.end(),[&](const PinPosition& pin){return pin.port==name.toStdString();});stored!=g.pin_positions.end())
                point={stored->x,stored->y};
            list.push_back({name,point});
        }
        ports(a, list, QColor("#17866d"));
    }
    for (const auto &g : project().plots) {
        auto *a = atom(g.id, g.name, {}, 3);
        a->differential_plot = g.differential;
        const auto port_count = g.differential ? g.inputs * 2 : g.inputs;
        if (a->input_count != port_count) {
            a->prepareGeometryChangeForInputs(port_count);
        }
        std::vector<std::pair<QString, QPointF>> list;
        for (unsigned i = 1; i <= g.inputs; ++i) {
            if (g.differential) {
                const double y = (2.0 * static_cast<double>(i) - static_cast<double>(port_count) - 1.0) * 10.0;
                list.push_back({"p" + QString::number(i), {-60, y}});
                list.push_back({"n" + QString::number(i), {-60, y + 20.0}});
            } else {
                const double y = (static_cast<double>(i) - (static_cast<double>(g.inputs) + 1.0) / 2.0) * 20.0;
                list.push_back({"in" + QString::number(i),
                                {-60, y}});
            }
        }
        for (auto &[name, point] : list)
            if (auto stored = std::find_if(g.pin_positions.begin(), g.pin_positions.end(), [&](const PinPosition &pin) {
                    return pin.port == name.toStdString();
                }); stored != g.pin_positions.end())
                point = {stored->x, stored->y};
        ports(a, list, QColor("#8c67c8"));
    }
    for (const auto &block : project().code_blocks) {
        auto *a = atom(block.id, block.name, QString("{C}"), 6);
        if(a->code_icon.empty())a->code_icon = block.icon;
        if (a->input_count != std::max(block.inputs.size(), block.outputs.size()))
            a->prepareGeometryChangeForInputs(unsigned(std::max(block.inputs.size(), block.outputs.size())));
        std::vector<std::pair<QString, QPointF>> list;
        auto append = [&](const std::vector<CodePort> &group, bool input) {
            for (size_t index = 0; index < group.size(); ++index) {
                const auto &port = group[index];
                const double y = (double(index) - double(group.size() - 1) / 2.0) * 28.0;
                const double x = a->code_port_distance();
                QPointF point{input ? -x : x, y};
                if (auto stored = std::find_if(block.pin_positions.begin(), block.pin_positions.end(),
                                               [&](const PinPosition &pin) { return pin.port == port.id; });
                    stored != block.pin_positions.end())
                    point = {stored->x, stored->y};
                const QString type = port.type == SignalScalarType::boolean ? QString("bool") : QString("double");
                const QString unit = port.unit.empty() ? QString() : QString(" [") + q(port.unit) + "]";
                list.push_back({q(port.name) + ": " + type + unit, point});
            }
        };
        append(block.inputs, true);
        append(block.outputs, false);
        std::vector<std::string> endpoint_ids;
        for (const auto &port : block.inputs) endpoint_ids.push_back(port.id);
        for (const auto &port : block.outputs) endpoint_ids.push_back(port.id);
        const auto children = a->childItems();
        bool endpoint_changed = children.size() != qsizetype(endpoint_ids.size());
        for (qsizetype i = 0; !endpoint_changed && i < children.size(); ++i)
            endpoint_changed = children[i]->data(2).toString().toStdString() != endpoint_ids[size_t(i)];
        if (endpoint_changed) {
            for (auto *child : a->childItems()) delete child;
            size_t index = 0;
            auto create = [&](const std::vector<CodePort> &group) {
                for (const auto &port : group) {
                    const auto &entry = list[index++];
                    a->port(q(port.id), entry.second,
                            port.type == SignalScalarType::boolean ? QColor("#17866d") : QColor("#8c67c8"),
                            entry.first);
                }
            };
            create(block.inputs);
            create(block.outputs);
        } else {
            size_t index = 0;
            for (auto *child : a->childItems()) {
                if (child->data(1).toString() != "port") continue;
                const auto &entry = list[index++];
                child->setPos(entry.second);
                child->setData(8, entry.first);
                child->setToolTip(entry.first);
                const auto &port = index <= block.inputs.size()
                    ? block.inputs[index - 1]
                    : block.outputs[index - block.inputs.size() - 1];
                const QColor color = port.type == SignalScalarType::boolean
                    ? QColor("#17866d") : QColor("#8c67c8");
                static_cast<PortDot *>(child)->setColor(color);
                child->setData(11, color.name(QColor::HexRgb));
            }
        }
    }
    for (const auto &i : project().instances) {
        atom(i.id, i.name, {}, 4)->set_definition(definition(project(), i.definition));
    }
    for (auto i = atoms_.begin(); i != atoms_.end();)
        if (!present.count(i->first)) {
            delete i->second;
            i = atoms_.erase(i);
        } else
            ++i;
    auto geometry = [&](const auto &objects) {
        for (const auto &o : objects) {
            auto *a = atoms_.at(o.id);
            a->setPos(o.x, o.y);
            a->setTransform(orientation_transform(o.orientation));
            a->update();
        }
    };
    geometry(project().components);
    geometry(project().nodes);
    geometry(project().tags);
    geometry(project().patterns);
    geometry(project().plots);
    geometry(project().code_blocks);
    geometry(project().instances);
    for (auto &[id, item] : atoms_) {
        (void)id;
        auto *atom_item = static_cast<Atom *>(item);
        if (atom_item->type == 3 || atom_item->type == 4) {
            const double grid = canvas_->grid_size();
            const double half_height = atom_item->type == 3
                ? std::max(40.0, atom_item->input_count * 20.0)
                : atom_item->body_half_height();
            const QRectF local_frame = atom_item->type == 3
                ? QRectF(-46, -half_height, 104, 2 * half_height)
                : QRectF(-90, -half_height, 180, 2 * half_height);
            auto transform = atom_item->transform();
            const QPointF origin = transform.map(QPointF());
            const double scale_x = QLineF(origin, transform.map(QPointF(1, 0))).length();
            const double scale_y = QLineF(origin, transform.map(QPointF(0, 1))).length();
            const double width = std::max(grid, std::round(local_frame.width() * scale_x / grid) * grid);
            const double height = std::max(grid, std::round(local_frame.height() * scale_y / grid) * grid);
            if (scale_x > 1e-9 && scale_y > 1e-9) {
                transform.scale(width / (local_frame.width() * scale_x),
                                height / (local_frame.height() * scale_y));
                atom_item->setTransform(transform);
            }
            const QRectF frame = atom_item->mapRectToScene(local_frame);
            const auto half_grid = [grid](double value) {
                return std::round((value - grid / 2.0) / grid) * grid + grid / 2.0;
            };
            atom_item->setPos(atom_item->pos() +
                              QPointF(half_grid(frame.left()) - frame.left(),
                                      half_grid(frame.top()) - frame.top()));
            atom_item->setData(14, local_frame);
        }
        // Port endpoints are always rendered on the world grid, including
        // transformed/scaled subcircuits. Collision repair below only changes
        // an exactly overlapping pin, so untouched neighbours stay in place.
        atom_item->snap_ports_to_grid(canvas_->grid_size());
        atom_item->separate_overlapping_ports(canvas_->grid_size());
    }
    update_labels();
    present.clear();
    for (const auto &w : project().wires) {
        present.insert(w.id);
        if (wires_.count(w.id))
            continue;
        auto *item = new WireItem;
        item->setFlag(QGraphicsItem::ItemIsSelectable);
        item->setData(0, q(w.id));
        item->setData(1, "wire");
        item->setZValue(-2);
        canvas_->scene()->addItem(item);
        wires_[w.id] = item;
    }
    for (auto i = wires_.begin(); i != wires_.end();)
        if (!present.count(i->first)) {
            delete i->second;
            i = wires_.erase(i);
        } else
            ++i;
    bool label_selected =
        std::any_of(labels_.begin(), labels_.end(), [](const auto &l) { return l.second->isSelected(); });
    if (!label_selected && atoms_.count(selected_))
        atoms_.at(selected_)->setSelected(true);
    if (wires_.count(selected_))
        wires_.at(selected_)->setSelected(true);
    auto bounds = canvas_->scene()->itemsBoundingRect().adjusted(-2000, -2000, 2000, 2000);
    canvas_->setSceneRect(bounds.united(QRectF(-10000, -10000, 20000, 20000)));
    update_wires();
}
QPointF EditorWindow::port_position(const Endpoint &e) const {
    auto it = atoms_.find(e.object);
    if (it == atoms_.end())
        return {};
    for (auto *child : it->second->childItems())
        if (child->data(2).toString() == q(e.port))
            return child->scenePos();
    return {};
}
QPointF EditorWindow::port_stub(const Endpoint &e, QPointF point) const {
    auto it = atoms_.find(e.object);
    if (it == atoms_.end())
        return point;
    auto *atom = static_cast<Atom *>(it->second);
    if (atom->type == 1)
        return point;
    QPointF delta(0, -20);
    if (atom->type == 4 || atom->type == 3 || atom->type == 6) {
        const QPointF local = atom->mapFromScene(point);
        const double h = atom->type == 4 ? atom->body_half_height()
                         : atom->type == 6 ? atom->natural_body_half_height()
                                           : std::max(40.0, atom->input_count * 20.0);
        const double vertical_edge_distance = atom->type == 6
            ? std::abs(std::abs(local.x()) - atom->code_port_distance())
            : atom->type == 4 ? std::abs(std::abs(local.x()) - 100.0)
            : std::min(std::abs(local.x() + 60.0), std::abs(local.x() - 60.0));
        const double horizontal_edge_distance = atom->type == 4 || atom->type == 6
            ? std::abs(std::abs(local.y()) - (h + 10.0))
            : std::abs(std::abs(local.y()) - (h + 20.0));
        if (vertical_edge_distance <= horizontal_edge_distance)
            delta = {local.x() < 0 ? -20. : 20., 0};
        else
            delta = {0, local.y() < 0 ? -20. : 20.};
    }
    else if (e.port == "p" || e.port.rfind("in", 0) == 0)
        delta = {-20, 0};
    else if (e.port == "n" || (e.port.rfind("out",0)==0 && atom->type == 2))
        delta = {20, 0};
    return point + atom->mapToScene(delta) - atom->mapToScene(QPointF());
}
QPainterPath EditorWindow::preview_route(Endpoint from, std::optional<Endpoint> to, QPointF a,
                                         QPointF b) const {
    std::vector<QRectF> obstacles;
    for (const auto &[id, box] : obstacle_cache_)
        obstacles.push_back(box);
    auto snap = [this](QPointF point) { return canvas_->snap_point(point); };
    return orthogonal_route(snap(a), snap(port_stub(from, a)), to ? snap(port_stub(*to, b)) : snap(b), snap(b),
                            obstacles);
}
void EditorWindow::update_wires() {
    if (!document_)
        return;
    bool all = topology_dirty_;
    bool routes_changed = all;
    std::set<std::string> changed_networks;
    if (topology_dirty_) {
        net_cache_ = resolve_connections(project()).nets;
        auto expanded = flatten(root_project());
        source_paths_ = std::move(expanded.origins);
        signal_sources_.clear();
        if (!hierarchy_path().empty()) {
            auto global = resolve_connections(root_project()).nets;
            for (auto &[key, net] : net_cache_) {
                auto slash = key.find('/');
                const auto expanded_key =
                    expanded_uuid(hierarchy_path(), key.substr(0, slash)) + key.substr(slash);
                if (auto found = global.find(expanded_key); found != global.end())
                    net = found->second;
            }
        }
        port_types_.clear();
        for (const auto &w : project().wires) {
            port_types_[endpoint_key(w.from)] = port_type(project(), w.from);
            port_types_[endpoint_key(w.to)] = port_type(project(), w.to);
            for (auto endpoint : {w.from, w.to}) {
                const auto key = endpoint_key(endpoint);
                endpoint.object = expanded_uuid(hierarchy_path(), endpoint.object);
                auto alias = expanded.terminals.find(endpoint_key(endpoint));
                signal_sources_[key] =
                    alias == expanded.terminals.end() ? endpoint.object : alias->second.object;
            }
        }
        topology_dirty_ = false;
    }
    std::vector<QRectF> obstacles, changed_boxes;
    std::map<std::string, QRectF> next_boxes;
    for (const auto &[id, item] : atoms_) {
        auto *atom = static_cast<Atom *>(item);
        if (atom->type == 1 || !atom->isVisible())
            continue;
        double h = atom->body_half_height();
        auto box =
            atom->mapRectToScene(atom->type == 4   ? QRectF(-90, -atom->body_half_height(), 180,
                                                            2 * atom->body_half_height())
                                 : atom->type == 3 ? QRectF(-46, -h, 104, 2 * h)
                                 : atom->type == 6 ? QRectF(-atom->code_body_half_width(), -h,
                                                            2 * atom->code_body_half_width(), 2 * h)
                                                   : QRectF(-38, -28, 76, 56));
        obstacles.push_back(box);
        next_boxes[id] = box;
        auto old = obstacle_cache_.find(id);
        if (old == obstacle_cache_.end() || old->second != box) {
            changed_boxes.push_back(box);
            if (old != obstacle_cache_.end())
                changed_boxes.push_back(old->second);
        }
    }
    for (const auto &[id, box] : obstacle_cache_)
        if (!next_boxes.count(id))
            changed_boxes.push_back(box);
    obstacle_cache_ = std::move(next_boxes);
    std::string highlight;
    const std::string channel = channels_ && channels_->currentItem()
                                    ? channels_->currentItem()->data(Qt::UserRole).toString().toStdString()
                                    : std::string();
    const bool gate_channel = channel.starts_with("gate/");
    const std::string channel_object = gate_channel ? channel.substr(5) : channel;
    std::string visible_source = channel_object;
    if (auto source = source_paths_.find(channel_object); source != source_paths_.end()) {
        const auto &path = source->second.instances;
        const auto &active = hierarchy_path();
        if (path.size() >= active.size() && std::equal(active.begin(), active.end(), path.begin()))
            visible_source = path.size() == active.size() ? source->second.object : path[active.size()];
        else
            visible_source.clear();
    }
    for (const auto &[id, atom] : atoms_) {
        const auto node = net_cache_.find(endpoint_key({id, "node"}));
        const bool active =
            !channel.empty() &&
            (id == visible_source || (!gate_channel && node != net_cache_.end() && node->second == channel));
        if (atom->data(channel_highlight_role).toBool() != active) {
            atom->setData(channel_highlight_role, active);
            atom->update();
        }
    }
    for (const auto &w : project().wires)
        if (w.id == selected_ && !wires_.at(w.id)->data(wire_segment_role).toInt()) {
            auto it = net_cache_.find(endpoint_key(w.from));
            if(it==net_cache_.end())it=net_cache_.find(endpoint_key(w.to));
            if (it != net_cache_.end())
                highlight = it->second;
        }
    std::map<std::string, const Wire *> previous;
    if (scene_project_)
        for (const auto &w : scene_project_->wires)
            previous[w.id] = &w;
    auto port_domain = [&](const Endpoint &endpoint) -> std::optional<Domain> {
        const auto found = port_types_.find(endpoint_key(endpoint));
        if (found == port_types_.end())
            return {};
        return found->second.domain;
    };
    for (const auto &w : project().wires) {
        auto *item = wires_.at(w.id);
        if (auto *wire_item = dynamic_cast<WireItem *>(item))
            wire_item->junctions.clear();
        const auto hidden_view = hidden_current_wire_view(project(), w.id);
        if (hidden_view && w.id == hidden_view->secondary) {
            item->setPath({});
            item->setVisible(false);
            continue;
        }
        item->setVisible(true);
        const Endpoint visual_from = hidden_view ? hidden_view->from : w.from;
        const Endpoint visual_to = hidden_view ? hidden_view->to : w.to;
        const auto from_domain_opt = port_domain(visual_from), to_domain_opt = port_domain(visual_to);
        if (!from_domain_opt || !to_domain_opt) {
            item->setPath({});
            continue;
        }
        auto a = canvas_->snap_point(port_position(visual_from)), b = canvas_->snap_point(port_position(visual_to));
        bool dirty = hidden_view.has_value() || all || item->path().isEmpty() ||
                     route_positions_[endpoint_key(visual_from)] != a ||
                     route_positions_[endpoint_key(visual_to)] != b;
        auto old = previous.find(w.id);
        if (old == previous.end() || old->second->bends != w.bends)
            dirty = true;
        if (!dirty && w.bends.empty())
            for (auto box : changed_boxes)
                if (box.intersects(item->path().boundingRect().adjusted(-25, -25, 25, 25))) {
                    dirty = true;
                    break;
                }
        if (dirty) {
            routes_changed = true;
            if (auto network = net_cache_.find(endpoint_key(w.from)); network != net_cache_.end())
                changed_networks.insert(network->second);
            auto bends = hidden_view ? hidden_view->bends : w.bends;
            if (canvas_->editing_gesture()) {
                const bool from_moving = atoms_.count(w.from.object) && atoms_.at(w.from.object)->isSelected();
                const bool to_moving = atoms_.count(w.to.object) && atoms_.at(w.to.object)->isSelected();
                if (from_moving != to_moving)
                    bends.clear();
                else
                    for (auto &point : bends)
                        point = canvas_->moving_point(w.from.object, w.to.object, point);
            }
            base_wire_routes_[w.id] =
                bends.empty() ? orthogonal_route(a, canvas_->snap_point(port_stub(visual_from, a)),
                                                 canvas_->snap_point(port_stub(visual_to, b)), b, obstacles)
                              : manual_route(a, b, bends);
            item->setPath(base_wire_routes_[w.id]);
        }
        const auto from_domain=*from_domain_opt;
        const auto to_domain=*to_domain_opt;
        const auto domain=from_domain==Domain::electrical&&to_domain==Domain::electrical?Domain::electrical:
            (from_domain==Domain::gate||to_domain==Domain::gate?Domain::gate:Domain::signal);
        auto net = net_cache_.find(endpoint_key(to_domain==Domain::electrical?w.to:w.from));
        bool active = (item->isSelected() && !item->data(wire_segment_role).toInt()) ||
                      (!highlight.empty() && net != net_cache_.end() && net->second == highlight);
        const bool channel_active =
            !channel.empty() && ((!gate_channel && net != net_cache_.end() && net->second == channel) ||
                                 (gate_channel && domain == Domain::gate &&
                                  (signal_sources_[endpoint_key(w.from)] == channel_object ||
                                   signal_sources_[endpoint_key(w.to)] == channel_object)));
        item->setData(channel_highlight_role, channel_active);
        const QColor default_color(domain == Domain::gate     ? "#17866d"
                                   : domain == Domain::signal ? "#8c67c8"
                                                              : "#146cca");
        const QColor wire_color = w.color.empty() ? default_color : QColor(QString::fromStdString(w.color));
        const auto default_line = domain == Domain::gate ? Qt::DashLine : Qt::SolidLine;
        const auto wire_line = w.line == WireLine::automatic
                                   ? default_line
                                   : w.line == WireLine::dash ? Qt::DashLine : Qt::SolidLine;
        item->setPen(QPen(channel_active ? QColor("#b34cce") : active ? QColor("#e88b22") : wire_color,
                          active || channel_active ? std::max(3.0, w.width + 1.0) : w.width, wire_line));
    }
    (void)routes_changed;
    (void)changed_networks;
    for (const auto &w : project().wires) {
        route_positions_[endpoint_key(w.from)] = canvas_->snap_point(port_position(w.from));
        route_positions_[endpoint_key(w.to)] = canvas_->snap_point(port_position(w.to));
    }
}
std::string EditorWindow::add_component(Kind kind, QPointF point) {
    if (running())
        return {};
    try {
        point = canvas_->snap_point(point);
        selected_ = document_->add_component(kind, point.x(), point.y());
        refresh_canvas(true, false);
        auto_connect_nearby_pins();
        return selected_;
    } catch (const std::exception &e) {
        show_error(e);
        return {};
    }
}
std::string EditorWindow::add_node(bool ground, QPointF point) {
    if (running())
        return {};
    try {
        point = canvas_->snap_point(point);
        selected_ = document_->add_node(ground, point.x(), point.y());
        refresh_canvas(true, false);
        auto_connect_nearby_pins();
        return selected_;
    } catch (const std::exception &e) {
        show_error(e);
        return {};
    }
}
std::string EditorWindow::add_pattern(QPointF point) {
    if (running())
        return {};
    try {
        point = canvas_->snap_point(point);
        selected_ = document_->add_pattern(point.x(), point.y());
        refresh_canvas(true, false);
        auto_connect_nearby_pins();
        return selected_;
    } catch (const std::exception &e) {
        show_error(e);
        return {};
    }
}
std::string EditorWindow::add_plot(QPointF point) {
    if (running())
        return {};
    point = canvas_->snap_point(point);
    selected_ = document_->add_plot(point.x(), point.y(), text("plot").toStdString());
    refresh_canvas(true, false);
    auto_connect_nearby_pins();
    return selected_;
}
std::string EditorWindow::add_code_block(QPointF point, const SignalPreset *preset) {
    if (running() || !editing_allowed())
        return {};
    try {
        point = canvas_->snap_point(point);
        CodeBlock block;
        if (preset) {
            const auto name = component_actions_.at(preset->placement_id)->text().toStdString();
            block = make_signal_preset(*preset, name, point.x(), point.y());
        } else {
            block.id = new_uuid();
            block.name = text("code_block").toStdString();
            block.x = point.x();
            block.y = point.y();
            block.code = "out = 0;";
            block.icon = default_code_icon(108);
            block.outputs.push_back({new_uuid(), "out", "", SignalScalarType::real, 0});
        }
        selected_ = block.id;
        document_->apply(preset ? "Add signal preset" : "Add code block", [&](Project &p) { p.code_blocks.push_back(block); });
        refresh_canvas(true, false);
        return selected_;
    } catch (const std::exception &error) {
        show_error(error);
        return {};
    }
}
bool EditorWindow::auto_connect_nearby_pins() {
    if (running() || rebuilding_)
        return false;
    struct Port {
        Endpoint endpoint;
        QPointF point;
        bool active = false;
    };
    std::set<std::string> active_objects;
    for (auto *item : canvas_->scene()->selectedItems()) {
        if (item->data(1).toString() == "wire" || item->data(1).toString() == "label")
            continue;
        const auto id = item->data(0).toString().toStdString();
        if (!id.empty())
            active_objects.insert(id);
    }
    if (!selected_.empty())
        active_objects.insert(selected_);
    std::vector<Port> ports;
    for (const auto &[id, atom] : atoms_)
        for (auto *child : atom->childItems())
            if (child->data(1).toString() == "port")
                ports.push_back({{child->data(0).toString().toStdString(),
                                  child->data(2).toString().toStdString()},
                                 child->scenePos(),
                                 active_objects.contains(child->data(0).toString().toStdString())});
    const bool has_active = std::any_of(ports.begin(), ports.end(), [](const Port &port) { return port.active; });
    std::vector<Wire> additions;
    const double threshold = std::max(3.0, canvas_->grid_size() * 0.35);
    auto exists = [&](const Endpoint &a, const Endpoint &b) {
        return std::any_of(project().wires.begin(), project().wires.end(), [&](const Wire &w) {
            return (w.from == a && w.to == b) || (w.from == b && w.to == a);
        });
    };
    for (size_t i = 0; i < ports.size(); ++i)
        for (size_t j = i + 1; j < ports.size(); ++j) {
            if (has_active && !ports[i].active && !ports[j].active)
                continue;
            if (ports[i].endpoint.object == ports[j].endpoint.object || exists(ports[i].endpoint, ports[j].endpoint))
                continue;
            if (QLineF(ports[i].point, ports[j].point).length() > threshold)
                continue;
            try {
                Wire wire{new_uuid(), ports[i].endpoint, ports[j].endpoint, {}};
                validate_wire(project(), wire);
                additions.push_back(std::move(wire));
            } catch (...) {
            }
        }
    if (additions.empty())
        return false;
    try {
        document_->apply("Auto-connect pins", [&](Project &p) {
            for (auto wire : additions) {
                try {
                    validate_wire(p, wire);
                } catch (...) {
                    continue;
                }
                if (std::none_of(p.wires.begin(), p.wires.end(), [&](const Wire &w) {
                        return (w.from == wire.from && w.to == wire.to) || (w.from == wire.to && w.to == wire.from);
                    }))
                    p.wires.push_back(std::move(wire));
            }
        });
        refresh_canvas();
        return true;
    } catch (const std::exception &e) {
        show_error(e);
        return false;
    }
}
bool EditorWindow::connect_ports(Endpoint from, Endpoint to) {
    if (running())
        return false;
    try {
        document_->connect(std::move(from), std::move(to));
        refresh_canvas();
        return true;
    } catch (const std::exception &e) {
        show_error(e);
        return false;
    }
}
void EditorWindow::commit_positions() {
    if (rebuilding_ || running())
        return;
    if (commit_label_positions())
        return;
    struct PortMove {
        std::string object;
        std::string definition;
        std::string port;
        double x = 0, y = 0;
        bool gate = false;
    };
    std::vector<PortMove> port_moves;
    for (const auto &instance : project().instances) {
        auto atom = atoms_.find(instance.id);
        if (atom == atoms_.end())
            continue;
        for (auto *child : atom->second->childItems()) {
            if (child->data(1).toString() != "port" || !child->data(9).toBool())
                continue;
            const auto point = child->pos();
            port_moves.push_back({instance.id, instance.definition, child->data(2).toString().toStdString(), point.x(), point.y(), false});
        }
    }
    for (const auto &plot : project().plots) {
        auto atom = atoms_.find(plot.id);
        if (atom == atoms_.end())
            continue;
        for (auto *child : atom->second->childItems()) {
            if (child->data(1).toString() != "port" || !child->data(9).toBool())
                continue;
            const auto point = child->pos();
            port_moves.push_back({plot.id, {}, child->data(2).toString().toStdString(), point.x(), point.y(), false});
        }
    }
    for (const auto &gate : project().patterns) {
        auto atom = atoms_.find(gate.id);
        if (atom == atoms_.end())
            continue;
        for (auto *child : atom->second->childItems()) {
            if (child->data(1).toString() != "port" || !child->data(9).toBool())
                continue;
            const auto point = child->pos();
            port_moves.push_back({gate.id, {}, child->data(2).toString().toStdString(), point.x(), point.y(), true});
        }
    }
    if (!port_moves.empty()) {
        document_->apply("Move public ports", [&](Project &p) {
            for (const auto &move : port_moves) {
                if (move.gate) {
                    auto gate = std::find_if(p.patterns.begin(), p.patterns.end(),
                                             [&](const GatePattern &candidate) { return candidate.id == move.object; });
                    if (gate == p.patterns.end())
                        continue;
                    auto pin = std::find_if(gate->pin_positions.begin(), gate->pin_positions.end(),
                                            [&](const PinPosition &candidate) { return candidate.port == move.port; });
                    if (pin == gate->pin_positions.end())
                        gate->pin_positions.push_back({move.port, move.x, move.y});
                    else {
                        pin->x = move.x;
                        pin->y = move.y;
                    }
                    continue;
                }
                if (move.definition.empty()) {
                    auto plot = std::find_if(p.plots.begin(), p.plots.end(),
                                             [&](const PlotBlock &candidate) { return candidate.id == move.object; });
                    if (plot == p.plots.end())
                        continue;
                    auto pin = std::find_if(plot->pin_positions.begin(), plot->pin_positions.end(),
                                            [&](const PinPosition &candidate) { return candidate.port == move.port; });
                    if (pin == plot->pin_positions.end())
                        plot->pin_positions.push_back({move.port, move.x, move.y});
                    else {
                        pin->x = move.x;
                        pin->y = move.y;
                    }
                    continue;
                }
                auto def = std::find_if(p.definitions.begin(), p.definitions.end(),
                                        [&](const auto &d) { return d.id == move.definition; });
                if (def == p.definitions.end())
                    continue;
                auto port = std::find_if(def->ports.begin(), def->ports.end(),
                                         [&](const auto &candidate) { return candidate.id == move.port; });
                if (port == def->ports.end())
                    continue;
                port->has_position = true;
                port->x = move.x;
                port->y = move.y;
            }
        });
        refresh_canvas(true, false);
        auto_connect_nearby_pins();
        return;
    }
    std::set<std::string> moved_ids;
    for (auto *item : canvas_->scene()->selectedItems()) {
        if (item->data(1).toString() == "wire" || item->data(1).toString() == "label")
            continue;
        if (!canvas_->gesture_contains(item))
            continue;
        const auto id = item->data(0).toString().toStdString();
        if (!id.empty())
            moved_ids.insert(id);
    }
    if (moved_ids.empty())
        return;
    bool moved = false;
    auto orientation = [](QGraphicsItem *item) {
        const auto t = item->transform();
        Orientation o;
        o.mirrored = t.determinant() < 0;
        o.scale = 1.0;
        o.scale_x = std::hypot(t.m11(), t.m12());
        o.scale_y = std::hypot(t.m21(), t.m22());
        o.quarter_turns =
            (static_cast<int>(std::lround(std::atan2(t.m12(), t.m22()) / (std::acos(-1.0) / 2))) + 4) % 4;
        return o;
    };
    auto check = [&](const auto &a) {
        if (!moved_ids.contains(a.id))
            return;
        if (atoms_.at(a.id)->pos() != QPointF(a.x, a.y) || orientation(atoms_.at(a.id)) != a.orientation)
            moved = true;
    };
    for (const auto &c : project().components)
        check(c);
    for (const auto &n : project().nodes)
        check(n);
    for (const auto &t : project().tags)
        check(t);
    for (const auto &g : project().patterns)
        check(g);
    for (const auto &g : project().plots)
        check(g);
    for (const auto &block : project().code_blocks)
        check(block);
    for (const auto &i : project().instances)
        check(i);
    if (!moved)
        return;
    document_->apply("Move objects", [&](Project &p) {
        auto live_position = [&](const Endpoint &endpoint) {
            return canvas_->snap_point(port_position(endpoint));
        };
        auto generated_node = [](const Node &node) {
            if (node.ground)
                return false;
            if (node.name.empty())
                return true;
            if (node.name[0] != 'N')
                return false;
            return std::all_of(node.name.begin() + 1, node.name.end(),
                               [](unsigned char c) { return std::isdigit(c); });
        };
        auto move_branch_nodes = [&] {
            for (auto &node : p.nodes) {
                if (!generated_node(node))
                    continue;
                std::vector<Wire *> incident;
                for (auto &wire : p.wires)
                    if (wire.from.object == node.id || wire.to.object == node.id)
                        incident.push_back(&wire);
                if (incident.size() < 3)
                    continue;
                Wire *branch = nullptr;
                Endpoint moved_endpoint;
                for (auto *wire : incident) {
                    const bool from_node = wire->from.object == node.id;
                    const Endpoint other = from_node ? wire->to : wire->from;
                    if (moved_ids.contains(other.object)) {
                        branch = wire;
                        moved_endpoint = other;
                        break;
                    }
                }
                if (!branch)
                    continue;
                struct TrunkEnd {
                    Wire *wire;
                    QPointF adjacent;
                    bool node_is_from;
                };
                std::vector<TrunkEnd> trunk;
                for (auto *wire : incident) {
                    if (wire == branch)
                        continue;
                    const auto graphics = wires_.find(wire->id);
                    if (graphics == wires_.end() || !graphics->second->isVisible())
                        continue;
                    const auto path = static_cast<QGraphicsPathItem *>(graphics->second)->path();
                    if (path.elementCount() < 2)
                        continue;
                    const bool node_is_from = wire->from.object == node.id;
                    const auto adjacent_element = path.elementAt(node_is_from ? 1 : path.elementCount() - 2);
                    trunk.push_back({wire, {adjacent_element.x, adjacent_element.y}, node_is_from});
                }
                if (trunk.size() < 2)
                    continue;
                const QPointF moved = live_position(moved_endpoint);
                const QPointF old_node(node.x, node.y);
                auto range_contains = [](double value, double a, double b) {
                    return value >= std::min(a, b) - 1e-6 && value <= std::max(a, b) + 1e-6;
                };
                std::optional<QPointF> target;
                for (size_t i = 0; i < trunk.size() && !target; ++i)
                    for (size_t j = i + 1; j < trunk.size() && !target; ++j) {
                        if (std::abs(trunk[i].adjacent.y() - old_node.y()) < 1e-6 &&
                            std::abs(trunk[j].adjacent.y() - old_node.y()) < 1e-6 &&
                            range_contains(moved.x(), trunk[i].adjacent.x(), trunk[j].adjacent.x()))
                            target = QPointF(moved.x(), old_node.y());
                        else if (std::abs(trunk[i].adjacent.x() - old_node.x()) < 1e-6 &&
                                 std::abs(trunk[j].adjacent.x() - old_node.x()) < 1e-6 &&
                                 range_contains(moved.y(), trunk[i].adjacent.y(), trunk[j].adjacent.y()))
                            target = QPointF(old_node.x(), moved.y());
                    }
                if (!target)
                    continue;
                target = canvas_->snap_point(*target);
                if (QLineF(QPointF(node.x, node.y), *target).length() < 1e-6)
                    continue;
                node.x = target->x();
                node.y = target->y();
                branch->bends.clear();
                for (const auto &end : trunk) {
                    const auto path = static_cast<QGraphicsPathItem *>(wires_.at(end.wire->id))->path();
                    QPainterPath adjusted;
                    for (int index = 0; index < path.elementCount(); ++index) {
                        const auto element = path.elementAt(index);
                        QPointF point(element.x, element.y);
                        if ((end.node_is_from && index == 0) ||
                            (!end.node_is_from && index == path.elementCount() - 1))
                            point = *target;
                        if (index == 0)
                            adjusted.moveTo(point);
                        else
                            adjusted.lineTo(point);
                    }
                    end.wire->bends = route_bends(adjusted);
                }
            }
        };
        for (auto &w : p.wires) {
            const bool from_moved = moved_ids.contains(w.from.object);
            const bool to_moved = moved_ids.contains(w.to.object);
            if (from_moved && to_moved) {
                for (auto &b : w.bends)
                    b = canvas_->moving_point(w.from.object, w.to.object, b);
                w.bends = clean_route_bends(port_position(w.from), port_position(w.to), w.bends);
            } else if (from_moved || to_moved) {
                w.bends.clear();
            }
        }
        auto move = [&](auto &a) {
            if (!moved_ids.contains(a.id))
                return;
            auto pos = atoms_.at(a.id)->pos();
            a.x = pos.x();
            a.y = pos.y();
            a.orientation = orientation(atoms_.at(a.id));
        };
        for (auto &c : p.components)
            move(c);
        for (auto &n : p.nodes)
            move(n);
        for (auto &t : p.tags)
            move(t);
        for (auto &g : p.patterns)
            move(g);
        for (auto &g : p.plots)
            move(g);
        for (auto &block : p.code_blocks)
            move(block);
        for (auto &i : p.instances)
            move(i);
        move_branch_nodes();
    });
    refresh_canvas(true, false);
    auto_connect_nearby_pins();
}
void EditorWindow::delete_selected() {
    canvas_->cancel_gesture();
    if (running())
        return;
    const auto selection = canvas_->scene()->selectedItems();
    for (auto *item : selection)
        if (item->data(1).toString() == "wire" && item->data(wire_segment_role).toInt() > 0) {
            if (selection.size() != 1)
                break;
            const auto id = item->data(0).toString().toStdString();
            const int segment = item->data(wire_segment_role).toInt();
            auto wire = std::find_if(project().wires.begin(), project().wires.end(),
                                     [&](const Wire &w) { return w.id == id; });
            if (wire == project().wires.end())
                continue;
            std::vector<Point> points;
            auto path = static_cast<QGraphicsPathItem *>(item)->path();
            for (int i = 0; i < path.elementCount(); ++i) {
                auto e = path.elementAt(i);
                points.push_back({e.x, e.y});
            }
            if (segment <= 0 || segment >= static_cast<int>(points.size()))
                continue;
            try {
                const auto cut_a = points[size_t(segment - 1)], cut_b = points[size_t(segment)];
                document_->apply("Delete wire segment", [&](Project &p) {
                    auto current =
                        std::find_if(p.wires.begin(), p.wires.end(), [&](const Wire &w) { return w.id == id; });
                    if (current == p.wires.end())
                        return;
                    const auto original = *current;
                    p.wires.erase(current);
                    auto make_node = [&](Point point) {
                        const auto node = new_uuid();
                        p.nodes.push_back({node, "N", false, point.x, point.y});
                        return node;
                    };
                    if (segment > 1) {
                        const auto node_a = make_node(cut_a);
                        Wire left{new_uuid(), original.from, {node_a, "node"}, {}};
                        left.color = original.color;
                        left.width = original.width;
                        left.line = original.line;
                        left.bends.assign(points.begin() + 1, points.begin() + segment - 1);
                        p.wires.push_back(std::move(left));
                    }
                    if (segment + 1 < static_cast<int>(points.size())) {
                        const auto node_b = make_node(cut_b);
                        Wire right{new_uuid(), {node_b, "node"}, original.to, {}};
                        right.color = original.color;
                        right.width = original.width;
                        right.line = original.line;
                        right.bends.assign(points.begin() + segment + 1, points.end() - 1);
                        p.wires.push_back(std::move(right));
                    }
                });
                selected_.clear();
                refresh_canvas();
            } catch (const std::exception &e) {
                show_error(e);
            }
            return;
    }
    std::vector<std::string> ids;
    for (auto *item : selection)
        if (item->data(1).toString() == "atom" || item->data(1).toString() == "wire")
            ids.push_back(item->data(0).toString().toStdString());
    if (ids.empty())
        return;
    if (ids.size() == 1)
        for (const auto &node : project().nodes)
            if (node.id == ids[0] && !node.ground) {
                std::vector<std::vector<Point>> routes;
                for (const auto &wire : project().wires)
                    if (wire.from.object == node.id || wire.to.object == node.id) {
                        std::vector<Point> route;
                        auto path = wires_.at(wire.id)->path();
                        for (int i = 0; i < path.elementCount(); ++i) {
                            auto e = path.elementAt(i);
                            route.push_back({e.x, e.y});
                        }
                        routes.push_back(route);
                    }
                if (routes.size() == 2) {
                    try {
                        document_->remove_junction(node.id, routes[0], routes[1]);
                        selected_.clear();
                        refresh_canvas();
                    } catch (const std::exception &e) {
                        show_error(e);
                    }
                    return;
                }
            }
    try {
        document_->erase(ids);
        selected_.clear();
        refresh_canvas();
    } catch (const std::exception &e) {
        show_error(e);
    }
}
void EditorWindow::undo() {
    bool gesture = canvas_->editing_gesture();
    canvas_->cancel_gesture();
    if (gesture)
        return;
    if (!running()) {

        document_->undo();
        refresh();
    }
}
void EditorWindow::redo() {
    bool gesture = canvas_->editing_gesture();
    canvas_->cancel_gesture();
    if (gesture)
        return;
    if (!running()) {

        document_->redo();
        refresh();
    }
}
void EditorWindow::select_object(const std::string &id) {
    std::string target = id;
    if (!atoms_.count(target) && !wires_.count(target)) {
        auto graph = resolve_connections(project());
        for (const auto &[endpoint, net] : graph.nets)
            if (net == id) {
                target = endpoint.substr(0, endpoint.find('/'));
                break;
            }
    }
    rebuilding_ = true;
    canvas_->scene()->clearSelection();
    QGraphicsItem *item =
        atoms_.count(target) ? atoms_.at(target) : (wires_.count(target) ? wires_.at(target) : nullptr);
    if (item) {
        item->setSelected(true);
        canvas_->ensureVisible(item);
    }
    selected_ = target;
    rebuilding_ = false;
    fill_inspector();
    update_wires();
    canvas_->setFocus();
    update_command_state();
}
void EditorWindow::copy_diagnostics() {
    QStringList messages;
    for (auto *item : errors_->selectedItems())
        messages.push_back(item->text());
    if (!messages.isEmpty())
        QApplication::clipboard()->setText(messages.join('\n'));
}
void EditorWindow::show_error(const std::exception &e) {
    QString message = QString::fromUtf8(e.what());
    auto *d = dynamic_cast<const Diagnostic *>(&e);
    if (d)
        message = q(d->code) + ": " + message;
    std::cerr << message.toStdString() << std::endl;
    qWarning().noquote() << message;
    auto *item = new QListWidgetItem(message, errors_);
    item->setData(Qt::UserRole + 2, QStringLiteral("error"));
    if (d)
        item->setData(Qt::UserRole, q(d->object));
    if (d) {
        QStringList path;
        for (const auto &step : d->path)
            path.push_back(q(step));
        item->setData(Qt::UserRole + 1, path);
    }
    bottom_->setCurrentIndex(0);
    banner_->setText(text("error_hint"));
}
void EditorWindow::show_warning(const QString &message) {
    std::cerr << message.toStdString() << std::endl;
    qWarning().noquote() << message;
    auto *item = new QListWidgetItem(style()->standardIcon(QStyle::SP_MessageBoxWarning), message, errors_);
    item->setData(Qt::UserRole + 2, QStringLiteral("warning"));
    bottom_->setCurrentIndex(0);
    banner_->setText(text("warning_hint"));
}
void EditorWindow::report_unhandled_error(const QString &message) {
    show_warning(message);
}
void EditorWindow::update_run_button() {
    run_->setText(text(!running() ? "run" : paused_.load() ? "resume" : "pause"));
    run_->setIcon(ui_icon(running() && !paused_.load() ? UiIcon::pause : UiIcon::play, true));
    auto *button = findChild<QToolButton *>("run_button");
    button->setStyleSheet(running() && !paused_.load()
                              ? "QToolButton#run_button{background:#dd8a18;color:white;}QToolButton#run_"
                                "button:hover{background:#bd6e0c;}"
                              : QString());
}
void EditorWindow::start_simulation() {
    if (running()) {
        if (!cancel_.load()) {
            paused_ = !paused_.load();
            update_run_button();
            update_command_state();
        }
        return;
    }
    launch_simulation(std::nullopt);
}
void EditorWindow::launch_simulation(std::optional<SimulationSnapshot> state, size_t max_steps) {
    if (running())
        return;
    simulation_timer_.start();
    if (!commit_inline_edit())
        return;
    auto restore_failed_dispatch = [this] {
        if (!busy_)
            return;
        cancel_ = true;
        paused_ = false;
        preparing_ = false;
        busy_ = false;
        if (simulation_progress_)
            simulation_progress_->hide();
        if (scope_)
            scope_->set_live(false);
        for (auto &[id, view] : plot_views_)
            if (view)
                view->set_live(false);
        canvas_->set_editable(editing_allowed());
        library_->setEnabled(editing_allowed());
        run_->setEnabled(true);
        scope_enable_->setEnabled(true);
        if (channels_)
            channels_->setEnabled(true);
        undo_->setEnabled(document_->can_undo());
        redo_->setEnabled(document_->can_redo());
        update_run_button();
        update_command_state();
    };
    try {
        Profile profile = project().profile;
        profile.stop = parse_si(stop_->text().toStdString(), "s");
        profile.step = parse_si(step_->text().toStdString(), "s");
        profile.method = method_->currentIndex() == 1 ? Method::trapezoidal : Method::backward_euler;
        if (profile.stop <= 0 || profile.step <= 0)
            throw std::runtime_error(text("positive_time").toStdString());
        document_->apply("Simulation profile", [&](Project &p) { p.profile = profile; });
        stop_->setText(QString::number(profile.stop, 'g', 12));
        step_->setText(QString::number(profile.step, 'g', 12));
        errors_->clear();
        const auto keys = recording_keys();
        std::set<std::string> recorded;
        if (result_) {
            for (const auto &channel : result_->channels)
                recorded.insert(channel.object);
            for (const auto &gate : result_->gate_objects)
                recorded.insert("gate/" + gate);
        }
        const bool append = state && result_ && result_->snapshot == state &&
                            recorded == std::set<std::string>(keys.begin(), keys.end());
        committed_sample_count_ = append && result_ ? result_->samples.size() : 0;
        continuation_statistics_ = append ? select_result(*result_, {}) : Result{};
        continuation_statistics_.snapshot.reset();
        if (!append)
            clear_result();
        if (!state)
            continuation_.reset();
        {
            std::lock_guard lock(stream_mutex_);
            stream_queue_.clear();
        }
        if (scope_)
            scope_->set_live(true);
        for (auto &[id, view] : plot_views_)
            if (view)
                view->set_live(true);
        result_project_ = root_project();
        scope_enable_->setEnabled(false);
        cancel_ = false;
        paused_ = false;
        simulated_time_ = state ? state->time : 0;
        preparing_ = true;

        canvas_->set_editable(false);
        library_->setEnabled(false);
        run_->setEnabled(true);
        undo_->setEnabled(false);
        redo_->setEnabled(false);
        banner_->setText(text("preparing") + " · " + text("elapsed").arg(0., 0, 'f', 2));
        if (simulation_progress_) {
            simulation_progress_->setRange(0, 0);
            simulation_progress_->setValue(0);
            simulation_progress_->setFormat(text("preparing"));
            simulation_progress_->show();
        }
        busy_ = true;
        update_run_button();
        update_command_state();
        const auto snapshot = root_project();
        const auto unexpected_error = text("unexpected_internal_error");
        watcher_.setFuture(QtConcurrent::run([this, snapshot, keys, state = std::move(state), max_steps,
                                              unexpected_error] {
            Outcome outcome;
            QElapsedTimer timer;
            timer.start();
            try {
                auto ir = compile(snapshot);
                auto catalog = available_channels(ir);
                Recording plan;
                plan.all = false;
                for (const auto &key : keys)
                    if (std::any_of(catalog.begin(), catalog.end(),
                                    [&](const Channel &c) { return c.object == key; }))
                        plan.channels.push_back(key);
                outcome.preparation_seconds = timer.nsecsElapsed() / 1e9;
                preparing_ = false;
                timer.restart();
                ExecutionOptions options;
                options.resume = state ? &*state : nullptr;
                options.capture_snapshot = true;
                options.max_steps = max_steps;
                options.stream_preview_samples = 4096;
                outcome.result =
                    execute(ir, &cancel_, &simulated_time_, &plan, &paused_, [this](Result &&batch) {
                        std::lock_guard lock(stream_mutex_);
                        stream_queue_.push_back(std::move(batch));
                    }, &options);
                outcome.execution_seconds = timer.nsecsElapsed() / 1e9;
            } catch (const Diagnostic &e) {
                outcome.error = q(e.code) + ": " + QString::fromUtf8(e.what());
                outcome.object = e.object;
                outcome.path = e.path;
            } catch (const std::exception &e) {
                outcome.error = QString::fromUtf8(e.what());
            } catch (...) {
                outcome.error = unexpected_error;
                outcome.warning = true;
            }
            return outcome;
        }));
        update_title();
    } catch (const std::exception &e) {
        restore_failed_dispatch();
        show_error(e);
    } catch (...) {
        restore_failed_dispatch();
        show_warning(text("unexpected_internal_error"));
    }
}
void EditorWindow::stop_simulation() {
    canvas_->cancel_gesture();
    if (running()) {
        discard_continuation_on_finish_ = true;
        step_after_pause_ = false;
        cancel_ = true;
        paused_ = false;
        run_->setEnabled(false);
        banner_->setText(text("stopping"));
    } else {

        placing_ = -1;
        library_->clearSelection();
        banner_->setText(text("hint"));
        update_wires();
    }
}
void EditorWindow::finish_simulation() {
    auto completed = watcher_.future();
    Outcome outcome;
    try {
        outcome = completed.takeResult();
    } catch (const std::exception &error) {
        outcome.error = QString::fromUtf8(error.what());
        outcome.warning = true;
    } catch (...) {
        outcome.error = text("unexpected_internal_error");
        outcome.warning = true;
    }
    drain_simulation_stream();
    busy_ = false;
    paused_ = false;
    if (simulation_progress_) {
        simulation_progress_->setRange(0, 1000);
        simulation_progress_->setValue(outcome.error.isEmpty() && outcome.result ? int(std::lround(std::clamp(
                                           outcome.result->last_time / std::max(outcome.result->profile.stop, 1e-30),
                                           0.0, 1.0) *
                                       1000.0))
                                     : 0);
        simulation_progress_->hide();
    }
    update_run_button();
    update_command_state();
    canvas_->set_editable(editing_allowed());
    library_->setEnabled(editing_allowed());
    run_->setEnabled(true);
    scope_enable_->setEnabled(true);
    if (channels_)
        channels_->setEnabled(true);
    undo_->setEnabled(document_->can_undo());
    redo_->setEnabled(document_->can_redo());
    if (!outcome.error.isEmpty()) {
        if (scope_)
            scope_->set_live(false);
        for (auto &[id, view] : plot_views_)
            if (view)
                view->set_live(false);
        if (outcome.warning)
            show_warning(outcome.error);
        else {
            auto *item = new QListWidgetItem(outcome.error, errors_);
            item->setData(Qt::UserRole, q(outcome.object));
            QStringList path;
            for (const auto &step : outcome.path)
                path.push_back(q(step));
            item->setData(Qt::UserRole + 1, path);
            bottom_->setCurrentIndex(0);
            banner_->setText(text("error_hint"));
        }
        return;
    }
    // Live batches are a bounded preview. Replace them with the complete worker
    // history (or trim them back to the committed continuation boundary).
    if (result_) {
        if (committed_sample_count_ == 0)
            result_.reset();
        else if (result_->samples.size() > committed_sample_count_)
            result_->samples.erase(result_->samples.begin() + static_cast<ptrdiff_t>(committed_sample_count_),
                                   result_->samples.end());
    }
    append_simulation_result(std::move(*outcome.result));
    const bool resume_one_step = step_after_pause_;
    step_after_pause_ = false;
    if (result_->cancelled && discard_continuation_on_finish_)
        continuation_.reset();
    else
        continuation_ = result_->snapshot;
    discard_continuation_on_finish_ = false;
    update_command_state();
    const bool stepped = !result_->cancelled && result_->last_time < result_->profile.stop;
    findChild<QLabel *>("diagnostic_status")
        ->setText(text(result_->cancelled ? "cancelled" : stepped ? "step_complete" : "diagnostics_ok") + " · " +
                  QString::number(result_->accepted_steps) + " " + text("steps"));
    choose_channels();
    update_graphs();
    if (scope_)
        scope_->set_live(false);
    for (auto &[id, view] : plot_views_)
        if (view)
            view->set_live(false);
    banner_->setText(text(result_->cancelled ? "cancelled" : stepped ? "step_complete" : "complete") + " · " +
                     QString::number(result_->accepted_steps) + " " + text("steps") + " · " +
                     text("simulated_at").arg(result_->last_time, 0, 'g', 8) + " · " +
                     text("elapsed").arg(simulation_timer_.elapsed() / 1000., 0, 'f', 2) +
                     (result_->samples.empty() ? " · " + text("no_recording") : QString()));
    banner_->setToolTip(text("simulation_timing")
                            .arg(outcome.preparation_seconds, 0, 'f', 3)
                            .arg(outcome.execution_seconds, 0, 'f', 3));
    if (result_->profile.step_control.adaptive) {
        banner_->setText(banner_->text() + " · " + text("adaptive_rejected").arg(result_->rejected_steps));
        banner_->setToolTip(banner_->toolTip() + "\n" + text("adaptive_statistics")
            .arg(result_->min_accepted_step, 0, 'g', 6).arg(result_->max_accepted_step, 0, 'g', 6)
            .arg(result_->max_local_error, 0, 'g', 4)
            .arg(text(("step_reason_" + (result_->step_reduction_reason.empty() ? std::string("none") : result_->step_reduction_reason)).c_str())));
    }
    update_title();
    if (resume_one_step && continuation_)
        launch_simulation(continuation_, 1);
}
void EditorWindow::choose_channels() {
    if (!channels_)
        return;
    std::vector<std::string> keys;
    const auto &colors = theme_colors().curves;
    bool prior = rebuilding_;
    rebuilding_ = true;
    for (int i = 0; i < channels_->count(); ++i) {
        auto *item = channels_->item(i);
        if (item->checkState() == Qt::Checked) {
            item->setForeground(colors[keys.size() % colors.size()]);
            keys.push_back(item->data(Qt::UserRole).toString().toStdString());
        } else
            item->setForeground(theme_colors().muted);
    }
    rebuilding_ = prior;
    if (keys != project().scope_channels)
        document_->apply("Scope channels", [&](Project &p) { p.scope_channels = keys; });
    if (scope_)
        scope_->set_result(result_ ? &*result_ : nullptr, result_indices(keys), project());
    scope_export_->setEnabled(result_ && !result_->samples.empty() && !result_indices(keys).empty());
    scope_fit_->setEnabled(result_ && !result_->samples.empty());
    update_title();
}
void EditorWindow::update_title() {
    if (!document_)
        return;
    setWindowTitle("PowerDriveSim — " +
                   (path_.isEmpty() ? q(root_project().name) : QFileInfo(path_).fileName()) +
                   (serialized(root_project()) == saved_state_ ? "" : " *"));
    const auto elements = project().components.size() + project().nodes.size() + project().tags.size() +
                          project().patterns.size() + project().plots.size() + project().code_blocks.size() +
                          project().instances.size();
    statusBar()->showMessage(QString::number(elements) + " " + text("components") + " · " +
                             QString::number(project().wires.size()) + " " + text("wires"));
}
bool EditorWindow::confirm_discard() {
    if (!commit_inline_edit())
        return false;
    if (!document_ || serialized(root_project()) == saved_state_)
        return true;
    auto choice = QMessageBox::question(this, text("unsaved"), text("save_question"),
                                        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (choice == QMessageBox::Cancel)
        return false;
    if (choice == QMessageBox::Discard)
        return true;
    auto path = path_;
    if (path.isEmpty())
        path = QFileDialog::getSaveFileName(this, text("save"), suggested_save_path(), text("project_filter"));
    return !path.isEmpty() && save_project(path);
}
void EditorWindow::closeEvent(QCloseEvent *e) {
    if (running()) {
        stop_simulation();
        e->ignore();
        return;
    }
    if (confirm_discard())
        e->accept();
    else
        e->ignore();
}
void EditorWindow::export_csv(const std::vector<std::string> &keys) {
    if (!result_)
        return;
    auto path = QFileDialog::getSaveFileName(this, text("export"), {}, "CSV (*.csv)");
    if (path.isEmpty())
        return;
    try {
        std::ostringstream stream;
        write_csv(select_result(*result_, keys), stream);
        auto bytes = stream.str();
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(bytes.data(), static_cast<qint64>(bytes.size())) !=
                static_cast<qint64>(bytes.size()) ||
            !file.commit())
            throw std::runtime_error(file.errorString().toStdString());
        banner_->setText(text("saved"));
    } catch (const std::exception &e) {
        show_error(e);
    }
}
} // namespace pds::desktop
