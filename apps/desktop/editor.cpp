#include "apps/desktop/editor.hpp"
#include "apps/desktop/labels.hpp"
#include "apps/desktop/number_input.hpp"
#include "apps/desktop/routing.hpp"
#include "apps/desktop/theme.hpp"
#include "apps/desktop/ui_icons.hpp"
#include "core/editor/properties.hpp"
#include "core/model/hierarchy.hpp"
#include "formats/project/project.hpp"
#include "results/csv.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
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
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
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
#include <sstream>
namespace pds::desktop {
static QString q(const std::string &s) {
    return QString::fromStdString(s);
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
static QPointF snapped(QPointF p) {
    return {std::round(p.x() / 20) * 20, std::round(p.y() / 20) * 20};
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
    QRectF boundingRect() const override { return {-7, -7, 14, 14}; }
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) override {
        if (parentItem() && parentItem()->data(4).isValid())
            return; // Junctions are drawn once by the parent, not as component terminals.
        if (hovered_) {
            painter->setPen(Qt::NoPen);
            auto halo = themed_signal(color_);
            halo.setAlpha(35);
            painter->setBrush(halo);
            painter->drawEllipse(QPointF(), 6, 6);
        }
        painter->setPen(QPen(themed_signal(color_), 1.2));
        painter->setBrush(theme_colors().surface);
        painter->drawEllipse(QPointF(), hovered_ ? 2.8 : 2.0, hovered_ ? 2.8 : 2.0);
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
    bool ground = false, separate_labels = false;
    Atom(std::string uuid, QString label, QString mark, int category)
        : id(std::move(uuid)), name(label), symbol(mark), type(category) {
        setData(0, q(id));
        setFlags(ItemIsSelectable | ItemSendsGeometryChanges);
        setZValue(2);
    }
    void prepareGeometryChangeForInputs(unsigned count) {
        prepareGeometryChange();
        input_count = count;
    }
    void set_definition(const Definition &definition) {
        if (definition_ports == definition.ports)
            return;
        prepareGeometryChange();
        definition_ports = definition.ports;
        public_ports.clear();
        for (auto *child : childItems())
            delete child;
        std::vector<const PublicPort *> left, right;
        unsigned electrical = 0;
        for (const auto &port : definition.ports) {
            bool on_right = port.direction == Direction::output ||
                            (port.direction == Direction::conserving && electrical++ % 2);
            (on_right ? right : left).push_back(&port);
        }
        input_count = unsigned(std::max(left.size(), right.size()));
        auto side = [&](const auto &group, double x) {
            for (size_t n = 0; n < group.size(); ++n) {
                QPointF pt(x, (double(n) - (group.size() - 1) / 2.) * 26);
                const auto &external = *group[n];
                public_ports.push_back({q(external.name), pt});
                port(q(external.id), pt,
                     QColor(external.domain == Domain::gate     ? "#17866d"
                            : external.domain == Domain::signal ? "#8c67c8"
                                                                : "#146cca"));
            }
        };
        side(left, -108);
        side(right, 108);
        for (auto *child : childItems())
            for (const auto &port : definition.ports)
                if (child->data(2).toString() == q(port.id))
                    child->setToolTip(q(port.name));
    }
    QRectF boundingRect() const override {
        if (type == 4) {
            double h = std::max(36., input_count * 14.);
            return {-110, -h - 8, 220, 2 * h + 40};
        }
        if (type == 3) {
            double h = std::max(36.0, input_count * 12.0);
            return {-80, -h - 14, 160, 2 * h + 52};
        }
        return type == 1 ? QRectF(-46, -12, 92, 68) : QRectF(-78, -53, 156, 104);
    }
    QPainterPath shape() const override {
        QPainterPath path;
        if (type == 4) {
            double h = std::max(36., input_count * 14.);
            path.addRect(QRectF(-90, -h, 180, 2 * h));
            return path;
        }
        if (type == 1) {
            if (ground)
                path.addRect(QRectF(-18, -5, 36, 34));
            else
                path.addEllipse(QPointF(), 7, 7);
            if (!separate_labels && !name.isEmpty())
                path.addRect(QRectF(-45, 31, 90, 20));
        } else if (type == 3) {
            double h = std::max(36.0, input_count * 12.0);
            path.addRect(QRectF(-46, -h, 104, 2 * h));
            if (!separate_labels)
                path.addRect(QRectF(-78, h + 7, 156, 24));
        } else {
            path.addRect(type == 2 ? QRectF(-38, -22, 76, 44) : QRectF(-29, -28, 58, 56));
            if (!separate_labels)
                path.addRect(QRectF(-77, 28, 154, 22));
            if (!separate_labels && !value.isEmpty())
                path.addRect(QRectF(-77, -51, 154, 20));
        }
        return path;
    }
    void port(const QString &port_name, QPointF location, const QColor &color) {
        auto *item = new PortDot(color, this);
        item->setPos(location);
        item->setData(0, q(id));
        item->setData(1, "port");
        item->setData(2, port_name);
        item->setToolTip(port_name);
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
    void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override {
        const bool channel_highlight = data(channel_highlight_role).toBool();
        p->setPen(QPen(channel_highlight ? QColor("#b34cce")
                       : isSelected()    ? QColor("#e88b22")
                                         : theme_colors().text,
                       channel_highlight ? 3 : 2));
        p->setBrush(theme_colors().surface);
        if (type == 4) {
            double h = std::max(36., input_count * 14.);
            p->setBrush(theme_colors().canvas);
            p->drawRoundedRect(QRectF(-90, -h, 180, 2 * h), 6, 6);
            auto font = p->font();
            font.setPointSize(8);
            p->setFont(font);
            for (const auto &[port_name, point] : public_ports) {
                p->drawLine(point, QPointF(point.x() < 0 ? -90 : 90, point.y()));
                label(p, QRectF(point.x() < 0 ? -86 : 6, point.y() - 10, 80, 20),
                      point.x() < 0 ? Qt::AlignLeft | Qt::AlignVCenter : Qt::AlignRight | Qt::AlignVCenter,
                      QFontMetricsF(font).elidedText(port_name, Qt::ElideRight, 65));
            }
            p->setPen(QPen(theme_colors().muted, 1.5));
            p->drawRect(QRectF(-8, -12, 16, 8));
            p->drawLine(0, -4, 0, 5);
            p->drawLine(-9, 5, 9, 5);
            p->drawRect(QRectF(-13, 5, 8, 8));
            p->drawRect(QRectF(5, 5, 8, 8));
            return;
        }
        if (type == 3) {
            double h = std::max(36.0, input_count * 12.0);
            p->setBrush(theme_colors().surface);
            p->setPen(QPen(isSelected() ? QColor("#3a7fe0") : theme_colors().border, 1.5));
            p->drawRoundedRect(QRectF(-46, -h, 104, 2 * h), 7, 7);
            p->setPen(QPen(theme_colors().grid, 1));
            p->drawLine(-24, 17, -24, -17);
            p->drawLine(-24, 17, 37, 17);
            p->setPen(QPen(theme_colors().signal, 2));
            QPainterPath curve;
            curve.moveTo(-20, 12);
            curve.cubicTo(-6, 10, -4, -15, 10, -11);
            curve.cubicTo(25, -8, 20, 7, 36, -4);
            p->drawPath(curve);
            for (unsigned i = 1; i <= input_count; ++i) {
                double y = (static_cast<double>(i) - (input_count + 1) / 2.0) * 22;
                p->setPen(QPen(theme_colors().signal, 1.4));
                p->drawLine(QPointF(-70, y), QPointF(-46, y));
                label(p, QRectF(-43, y - 9, 16, 18), Qt::AlignCenter, QString::number(i));
            }
            p->setPen(theme_colors().text);
            if (!separate_labels)
                label(p, QRectF(-78, h + 7, 156, 24), Qt::AlignCenter, name);
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
            p->setBrush(theme_colors().gate_fill);
            p->drawRoundedRect(QRectF(-38, -22, 76, 44), 7, 7);
            label(p, QRectF(-38, -22, 76, 44), Qt::AlignCenter, symbol.isEmpty() ? QString("Gate") : symbol);
            p->drawLine(38, 0, 60, 0);
        } else {
            p->drawLine(-60, 0, -27, 0);
            p->drawLine(27, 0, 60, 0);
            p->save();
            auto port_font = p->font();
            port_font.setPointSize(8);
            p->setFont(port_font);
            label(p, QRectF(-61, 4, 18, 16), Qt::AlignLeft, "p");
            label(p, QRectF(44, 4, 18, 16), Qt::AlignRight, "n");
            p->restore();
            if (symbol == "R")
                p->drawRect(QRectF(-27, -12, 54, 24));
            else if (symbol == "C") {
                p->drawLine(-27, 0, -7, 0);
                p->drawLine(7, 0, 27, 0);
                p->drawLine(-7, -22, -7, 22);
                p->drawLine(7, -22, 7, 22);
            } else if (symbol == "L") {
                for (int i = 0; i < 4; ++i)
                    p->drawArc(QRectF(-28 + i * 14, -14, 14, 28), 0, 180 * 16);
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
        for (const QString role : {QString("name"), QString("value")}) {
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
            label->setToolTip(text("label_hint"));
            const QPointF anchor = role == "value"   ? QPointF(0, -41)
                                   : atom->type == 1 ? QPointF(0, 41)
                                   : atom->type == 3 ? QPointF(0, std::max(36., atom->input_count * 12.) + 19)
                                   : atom->type == 4 ? QPointF(0, std::max(36., atom->input_count * 14.) + 19)
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
        if (item->isSelected() && !atoms_.at(item->data(0).toString().toStdString())->isSelected()) {
            const auto id = item->data(0).toString().toStdString();
            auto pos = atoms_.at(id)->mapFromScene(item->pos()) - item->data(5).toPointF();
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
        if (item->isSelected() && !atoms_.at(item->data(0).toString().toStdString())->isSelected()) {
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
        int c[] = {1, 0, -1, 0}, sn[] = {0, 1, 0, -1};
        auto t = o.orientation.quarter_turns % 4;
        double sign = o.orientation.mirrored ? -1 : 1;
        a->setTransform(QTransform(sign * c[t], sn[t], -sign * sn[t], c[t], 0, 0));
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
        auto *a = add(n, 1, {}, {});
        a->ground = n.ground;
        a->port("node", {0, 0}, QColor("#146cca"));
    }
    for (const auto &g : fragment.patterns) {
        auto *a = add(g, 2, g.pwm ? QString("PWM") : QString(),
                      g.pwm ? QString::number(g.frequency) + " Hz · " + QString::number(g.duty * 100) + " %"
                            : QString());
        a->port("out", {60, 0}, QColor("#17866d"));
    }
    for (const auto &g : fragment.plots) {
        auto *a = add(g, 3, {}, {});
        a->input_count = g.inputs;
        for (unsigned i = 1; i <= g.inputs; ++i)
            a->port("in" + QString::number(i), {-70, (static_cast<double>(i) - (g.inputs + 1) / 2.0) * 22},
                    QColor("#8c67c8"));
    }
    for (const auto &instance : fragment.instances)
        add(instance, 4, {}, {})->set_definition(definition(fragment, instance.definition));
    auto position = [&](const Endpoint &endpoint) {
        for (auto *child : group->childItems())
            if (auto *a = dynamic_cast<Atom *>(child); a && a->id == endpoint.object)
                for (auto *port : a->childItems())
                    if (port->data(2).toString() == q(endpoint.port))
                        return port->mapToItem(group, QPointF());
        return QPointF();
    };
    std::vector<QRectF> obstacles;
    for (auto *child : group->childItems())
        obstacles.push_back(child->mapRectToParent(QRectF(-38, -28, 76, 56)));
    for (const auto &wire : fragment.wires) {
        auto a = position(wire.from), b = position(wire.to);
        auto path =
            wire.bends.empty() ? orthogonal_route(a, a, b, b, obstacles) : manual_route(a, b, wire.bends);
        auto *item = new QGraphicsPathItem(path);
        item->setPen(QPen(QColor("#146cca"), 2));
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
                                  : a->type == 3  ? std::max(36., a->input_count * 12.) + 19
                                  : a->type == 4  ? std::max(36., a->input_count * 14.) + 19
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
        else if (placing_ == 102 || placing_ == 104) {
            fragment.add_pattern(0, 0);
            if (placing_ == 104)
                fragment.apply("PWM", [](Project &p) {
                    p.patterns[0].pwm = true;
                    p.patterns[0].name = "PWM";
                });
        } else if (placing_ == 103)
            fragment.add_plot(0, 0, text("plot").toStdString());
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
    try {
        auto ids = document_->paste(*paste_fragment_, point.x(), point.y());
        canvas_->set_ghost(nullptr);
        cancel_placement();
        canvas_->scene()->clearSelection();
        selected_ = ids.empty() ? "" : ids.front();
        refresh();
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
    connect(&watcher_, &QFutureWatcher<Outcome>::finished, this, [this] { finish_simulation(); });
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { autosave(); });
    timer->start(15000);
    auto *progress_timer = new QTimer(this);
    connect(progress_timer, &QTimer::timeout, this, [this] {
        if (running())
            drain_simulation_stream();
        if (running() && !cancel_.load())
            banner_->setText(
                text(paused_.load()      ? "paused"
                     : preparing_.load() ? "preparing"
                                         : "running") +
                "  t = " + QString::number(simulated_time_.load(std::memory_order_relaxed), 'g', 6) + " / " +
                QString::number(project().profile.stop, 'g', 6) + " s · " +
                text("elapsed").arg(simulation_timer_.elapsed() / 1000., 0, 'f', 2));
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
    action(file_menu, "new", QKeySequence::New, [this] {
        if (!running() && confirm_discard())
            new_file();
    });
    action(file_menu, "open", QKeySequence::Open, [this] {
        if (running() || !confirm_discard())
            return;
        auto f = QFileDialog::getOpenFileName(this, text("open"), {}, text("project_filter"));
        if (!f.isEmpty())
            open_project(f);
    });
    action(file_menu, "save", QKeySequence::Save, [this] {
        auto f = path_;
        if (f.isEmpty())
            f = QFileDialog::getSaveFileName(this, text("save"), {}, text("project_filter"));
        if (!f.isEmpty())
            save_project(f);
    });
    file_menu->addSeparator();
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
    action(edit_menu, "delete", QKeySequence::Delete, [this] { delete_selected(); });
    action(edit_menu, "rotate", QKeySequence("Space"), [this] { transform_selection(1, false); });
    action(edit_menu, "rotate_back", QKeySequence("Shift+Space"), [this] { transform_selection(-1, false); });
    action(edit_menu, "mirror", QKeySequence("Ctrl+M"), [this] { transform_selection(0, true); });
    action(edit_menu, "copy", QKeySequence::Copy, [this] { copy_selection(false); });
    action(edit_menu, "cut", QKeySequence::Cut, [this] { copy_selection(true); });
    action(edit_menu, "paste", QKeySequence::Paste, [this] { paste_selection(); });
    action(edit_menu, "duplicate", QKeySequence("Ctrl+D"), [this] { paste_selection(true); });
    action(edit_menu, "select_all", QKeySequence::SelectAll, [this] {
        for (auto &[id, item] : atoms_)
            item->setSelected(true);
    });
    action(edit_menu, "properties", QKeySequence("Alt+Return"), [this] {
        fill_inspector();
        if (name_ && name_->isVisible()) {
            name_->setFocus();
            name_->selectAll();
        }
    });
    for (const char *mode : {"left", "right", "top", "bottom", "horizontal", "vertical"})
        action(nullptr, mode, {}, [this, mode] { arrange_selection(mode); });
    action(edit_menu, "shortcuts", {}, [this] { show_shortcuts(); });
    build_hierarchy_actions(edit_menu->addMenu(text("hierarchy")));
    auto *wire_action = action(edit_menu, "connect_tool", QKeySequence("Ctrl+W"), [this] {
        if (running())
            return;
        canvas_->cancel_gesture();
        placing_ = 101;
        set_placement_preview();
        banner_->setText(text("junction_hint"));
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
    auto *examples = menuBar()->addMenu(text("examples"));
    QDir dir(QCoreApplication::applicationDirPath() + "/examples");
    for (const auto &file : dir.entryList({"*.pds"}, QDir::Files))
        connect(examples->addAction(file), &QAction::triggered, this, [this, dir, file] {
            if (!running() && confirm_discard())
                open_project(dir.filePath(file));
        });
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
    auto *stop_button = new QToolButton;
    stop_button->setObjectName("stop_button");
    stop_button->setDefaultAction(stop_action_);
    stop_action_->setIcon(ui_icon(UiIcon::stop, true));
    stop_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    stop_button->setIconSize({16, 16});
    stop_button->setFixedSize(132, 36);
    toolbar->addWidget(stop_button);
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
    auto *title = new QLabel(text("canvas_title"));
    breadcrumbs_ = title;
    title->setObjectName("hierarchy_breadcrumbs");
    title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    connect(title, &QLabel::linkActivated, this, [this](const QString &link) {
        auto path = hierarchy_path();
        bool valid = false;
        auto depth = link.toUInt(&valid);
        if (valid && depth <= path.size()) {
            path.resize(depth);
            navigate_hierarchy(path);
        }
    });
    title->setStyleSheet("font-weight:600;border:0;padding-right:16px;");
    tools_layout->addWidget(title, 1);
    for (auto *a : {wire_action, undo_, redo_, fit_action}) {
        auto *button = new QToolButton;
        button->setDefaultAction(a);
        tools_layout->addWidget(button);
    }
    layout->addWidget(canvas_tools);
    canvas_ = new Canvas;
    layout->addWidget(canvas_, 1);
    banner_ = new QLabel(text("hint"));
    banner_->setStyleSheet(
        "padding:9px 16px;color:palette(placeholder-text);background:palette(window);border-top:1px solid "
        "palette(mid);");
    banner_->setWordWrap(true);
    layout->addWidget(banner_);
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
    library_->setHeaderHidden(true);
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
    auto *inspector = new QWidget;
    properties_ = new QFormLayout(inspector);
    properties_->setContentsMargins(16, 18, 16, 18);
    properties_->setVerticalSpacing(14);
    inspector_hint_ = new QLabel(text("inspector_empty"));
    inspector_hint_->setWordWrap(true);
    inspector_hint_->setStyleSheet("color:palette(placeholder-text);padding:18px 0;");
    properties_->addRow(inspector_hint_);
    properties_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    build_property_editors();
    auto *apply = new QPushButton(text("apply"));
    apply->setObjectName("apply_properties");
    apply_button_ = apply;
    properties_->addRow(apply);
    property_error_ = new QLabel;
    property_error_->setObjectName("property_error");
    property_error_->setStyleSheet("color:palette(bright-text)");
    property_error_->setWordWrap(true);
    properties_->addRow(property_error_);
    connect(apply, &QPushButton::clicked, this, [this] { apply_inspector(); });
    for (auto *field : {stop_, step_})
        connect(field, &QLineEdit::editingFinished, this, [this] { commit_profile(); });
    connect(method_, &QComboBox::activated, this, [this] { commit_profile(); });
    auto *right = dock("inspector", inspector, Qt::RightDockWidgetArea);
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
    dl->addWidget(errors_, 1);
    bottom_->addTab(diagnostics, text("diagnostics"));
    connect(errors_, &QListWidget::itemClicked, this, [this](QListWidgetItem *i) {
        std::vector<std::string> path;
        for (const auto &step : i->data(Qt::UserRole + 1).toStringList())
            path.push_back(step.toStdString());
        navigate_hierarchy(path);
        select_object(i->data(Qt::UserRole).toString().toStdString());
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
    scope_layout_->addWidget(scope_hint_, 1);
    bottom_->addTab(scope_page_, text("scope_tab"));
    connect(scope_enable_, &QCheckBox::toggled, this, [this](bool checked) {
        if (!rebuilding_ && document_)
            set_scope_enabled(checked);
    });
    connect(scope_export_, &QPushButton::clicked, this, [this] { export_csv(project().scope_channels); });
    auto *bottom_dock = dock("results", bottom_, Qt::BottomDockWidgetArea);
    bottom_dock->setTitleBarWidget(new QWidget);
    bottom_dock->setMinimumHeight(140);
    resizeDocks({bottom_dock}, {180}, Qt::Vertical);
    connect(bottom_, &QTabWidget::currentChanged, this, [this, bottom_dock](int tab) {
        bottom_dock->setMinimumHeight(tab == 1 ? 320 : 140);
        if (!bottom_dock->isFloating())
            resizeDocks({bottom_dock}, {tab == 1 ? 360 : 180}, Qt::Vertical);
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
    canvas_->connect_wire = [this](WireAnchor from, WireAnchor to, std::vector<Point> bends,
                                   std::string replace) {
        connect_gesture(from, to, std::move(bends), replace);
    };
    canvas_->route_preview = [this](Endpoint a, std::optional<Endpoint> b, QPointF start, QPointF end) {
        return preview_route(a, b, start, end);
    };
    canvas_->wire_endpoint = [this](const std::string &id, bool from) {
        for (const auto &w : project().wires)
            if (w.id == id)
                return from ? w.from : w.to;
        return Endpoint{};
    };
    canvas_->edit_route = [this](std::string id, std::vector<Point> bends) {
        if (running())
            return;
        try {
            document_->apply("Edit route", [&](Project &p) {
                for (auto &w : p.wires)
                    if (w.id == id)
                        w.bends = bends;
            });
            refresh(false);
        } catch (const std::exception &e) {
            show_error(e);
            refresh(false);
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
        update_labels();
        update_wires();
    };
    canvas_->released = [this] { commit_positions(); };
    canvas_->open_object = [this](std::string id) {
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
    document_ = std::make_unique<Document>(std::move(p));
    delete scope_content_;
    scope_content_ = nullptr;
    scope_ = nullptr;
    channels_ = nullptr;
    stop_->setModified(false);
    step_->setModified(false);
    selected_.clear();

    path_.clear();
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
        set_project(read_project(in));
        path_ = path;
        update_title();
        banner_->setText(text("hint"));
        return true;
    } catch (const std::exception &e) {
        show_error(e);
        return false;
    }
}
bool EditorWindow::save_project(const QString &path) {
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
bool EditorWindow::recover(const QString &path) {
    if (!open_project(path))
        return false;
    path_.clear();
    saved_state_.clear();
    update_title();
    banner_->setText(text("recovered"));
    return true;
}
void EditorWindow::refresh(bool invalidate) {
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
    update_instance_specs();
    const bool model_changed = !scene_project_ || !same_simulation(*scene_project_, project());
    auto labels = [](const Project &p) {
        std::map<std::string, std::string> result;
        auto collect = [&](const auto &objects) {
            for (const auto &o : objects)
                result[o.id] = o.name;
        };
        collect(p.nodes);
        collect(p.components);
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
        item(c.id, c.name);
    for (const auto &n : project().nodes)
        item(n.id, n.name);
    for (const auto &g : project().patterns)
        item(g.id, g.name);
    for (const auto &g : project().plots)
        item(g.id, g.name);
    for (const auto &i : project().instances)
        item(i.id, i.name);
    if (!stop_->isModified())
        stop_->setText(QString::number(project().profile.stop, 'g', 12));
    if (!step_->isModified())
        step_->setText(QString::number(project().profile.step, 'g', 12));
    method_->setCurrentIndex(project().profile.method == Method::trapezoidal ? 1 : 0);
    undo_->setEnabled(document_->can_undo() && !running());
    redo_->setEnabled(document_->can_redo() && !running());
    sync_scope();
    if (model_changed || labels_changed || recording_changed)
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
    update_graphs();
    scene_project_ = project();
    rebuilding_ = false;
    fill_inspector();
    refresh_hierarchy();
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
                    if (child->data(2).toString() == name)
                        child->setPos(point);
    };
    for (const auto &c : project().components) {
        auto *a = atom(c.id, c.name, q(kind_name(c.kind)), 0);
        a->value = component_label(c);
        std::vector<std::pair<QString, QPointF>> list{{"p", {-60, 0}}, {"n", {60, 0}}};
        if (gate_controlled(c.kind))
            list.push_back({"gate", {0, -40}});
        if (c.kind == Kind::voltage_probe || c.kind == Kind::current_probe)
            list.push_back({"out", {0, -40}});
        ports(a, list, QColor("#146cca"));
    }
    for (const auto &n : project().nodes) {
        auto *a = atom(n.id, n.name, {}, 1);
        a->ground = n.ground;
        int degree = 0;
        for (const auto &w : project().wires)
            degree += w.from.object == n.id || w.to.object == n.id;
        a->setData(4, degree);
        a->setToolTip(text(n.ground ? "ground" : degree >= 3 ? "junction_branch" : "junction_pass"));
        ports(a, {{"node", {0, 0}}}, QColor("#146cca"));
    }
    for (const auto &g : project().patterns) {
        auto *a = atom(g.id, g.name, g.pwm ? QString("PWM") : QString(), 2);
        a->value = g.pwm ? QString::number(g.frequency) + " Hz · " + QString::number(g.duty * 100) + " %"
                         : QString();
        ports(a, {{"out", {60, 0}}}, QColor("#17866d"));
    }
    for (const auto &g : project().plots) {
        auto *a = atom(g.id, g.name, {}, 3);
        if (a->input_count != g.inputs) {
            a->prepareGeometryChangeForInputs(g.inputs);
        }
        std::vector<std::pair<QString, QPointF>> list;
        for (unsigned i = 1; i <= g.inputs; ++i)
            list.push_back(
                {"in" + QString::number(i), {-70, (static_cast<double>(i) - (g.inputs + 1) / 2.0) * 22}});
        ports(a, list, QColor("#8c67c8"));
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
            int c[] = {1, 0, -1, 0}, sn[] = {0, 1, 0, -1};
            unsigned t = o.orientation.quarter_turns % 4;
            double sign = o.orientation.mirrored ? -1 : 1;
            a->setTransform(QTransform(sign * c[t], sn[t], -sign * sn[t], c[t], 0, 0));
            a->update();
        }
    };
    geometry(project().components);
    geometry(project().nodes);
    geometry(project().patterns);
    geometry(project().plots);
    geometry(project().instances);
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
    if (atom->type == 4)
        delta = {atom->mapFromScene(point).x() < 0 ? -20. : 20., 0};
    else if (e.port == "p" || e.port.rfind("in", 0) == 0)
        delta = {-20, 0};
    else if (e.port == "n" || (e.port == "out" && atom->type == 2))
        delta = {20, 0};
    return point + atom->mapToScene(delta) - atom->mapToScene(QPointF());
}
QPainterPath EditorWindow::preview_route(Endpoint from, std::optional<Endpoint> to, QPointF a,
                                         QPointF b) const {
    std::vector<QRectF> obstacles;
    for (const auto &[id, box] : obstacle_cache_)
        obstacles.push_back(box);
    return orthogonal_route(a, port_stub(from, a), to ? port_stub(*to, b) : b, b, obstacles);
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
        if (atom->type == 1)
            continue;
        double h = std::max(36.0, atom->input_count * 12.0);
        auto box =
            atom->mapRectToScene(atom->type == 4   ? QRectF(-90, -std::max(36., atom->input_count * 14.), 180,
                                                            2 * std::max(36., atom->input_count * 14.))
                                 : atom->type == 3 ? QRectF(-46, -h, 104, 2 * h)
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
    for (const auto &w : project().wires) {
        auto *item = wires_.at(w.id);
        auto a = port_position(w.from), b = port_position(w.to);
        bool dirty = all || item->path().isEmpty() || route_positions_[endpoint_key(w.from)] != a ||
                     route_positions_[endpoint_key(w.to)] != b;
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
            auto bends = w.bends;
            if (canvas_->editing_gesture())
                for (auto &point : bends) {
                    point = canvas_->moving_point(w.from.object, w.to.object, point);
                }
            base_wire_routes_[w.id] =
                bends.empty() ? orthogonal_route(a, port_stub(w.from, a), port_stub(w.to, b), b, obstacles)
                              : manual_route(a, b, bends);
            item->setPath(base_wire_routes_[w.id]);
        }
        const auto from_domain=port_types_.at(endpoint_key(w.from)).domain;
        const auto to_domain=port_types_.at(endpoint_key(w.to)).domain;
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
        item->setPen(QPen(channel_active ? QColor("#b34cce")
                          : active       ? QColor("#e88b22")
                                         : QColor(domain == Domain::gate     ? "#17866d"
                                                  : domain == Domain::signal ? "#8c67c8"
                                                                             : "#146cca"),
                          active || channel_active ? 3 : 2,
                          domain == Domain::gate ? Qt::DashLine : Qt::SolidLine));
    }
    if (routes_changed)
        share_wire_trunks(obstacles, changed_networks);
    for (const auto &w : project().wires) {
        route_positions_[endpoint_key(w.from)] = port_position(w.from);
        route_positions_[endpoint_key(w.to)] = port_position(w.to);
    }
}
void EditorWindow::share_wire_trunks(const std::vector<QRectF> &obstacles,
                                     const std::set<std::string> &networks) {
    std::vector<const Wire *> ordered;
    for (const auto &wire : project().wires) {
        const auto network = net_cache_.find(endpoint_key(wire.from));
        if (network == net_cache_.end() || !networks.contains(network->second))
            continue;
        auto *item = static_cast<WireItem *>(wires_.at(wire.id));
        item->junctions.clear();
        if (auto found = base_wire_routes_.find(wire.id); found != base_wire_routes_.end())
            item->setPath(found->second);
        if (port_types_.at(endpoint_key(wire.from)).domain == Domain::electrical &&
            port_types_.at(endpoint_key(wire.to)).domain == Domain::electrical)
            ordered.push_back(&wire);
    }
    auto rank = [&](const Wire *wire) {
        const auto a = port_position(wire->from), b = port_position(wire->to);
        return std::make_tuple(wire->bends.empty(),
                               std::min(std::abs(a.x() - b.x()), std::abs(a.y() - b.y())),
                               base_wire_routes_.at(wire->id).length(), wire->id);
    };
    std::sort(ordered.begin(), ordered.end(),
              [&](const Wire *a, const Wire *b) { return rank(a) < rank(b); });
    struct Edge {
        std::string to;
        QPainterPath path;
    };
    std::map<std::string, std::vector<Edge>> network;
    for (const auto *wire : ordered) {
        auto *item = static_cast<WireItem *>(wires_.at(wire->id));
        if (wire->bends.empty()) {
            double best_added = item->path().length();
            std::optional<std::pair<QPainterPath, QPointF>> best;
            for (bool reverse : {false, true}) {
                const auto root = endpoint_key(reverse ? wire->from : wire->to);
                const auto branch = reverse ? item->path().toReversed() : item->path();
                const QPointF start(branch.elementAt(0).x, branch.elementAt(0).y);
                const auto stub = port_stub(reverse ? wire->to : wire->from, start);
                std::set<std::string> visited{root};
                std::vector<std::pair<std::string, QPainterPath>> pending{
                    {root, QPainterPath(branch.currentPosition())}};
                for (size_t i = 0; i < pending.size(); ++i) {
                    const auto current = pending[i];
                    auto found = network.find(current.first);
                    if (found == network.end())
                        continue;
                    for (const auto &edge : found->second) {
                        if (!visited.insert(edge.to).second)
                            continue;
                        auto path = current.second;
                        path.connectPath(edge.path);
                        pending.emplace_back(edge.to, path);
                        auto joined = join_route_to_trunk(branch, stub, path.toReversed(), obstacles);
                        if (!joined)
                            continue;
                        double added = 0;
                        for (int k = 1; k < joined->first.elementCount(); ++k) {
                            auto a = joined->first.elementAt(k - 1), b = joined->first.elementAt(k);
                            added += QLineF(QPointF(a.x, a.y), QPointF(b.x, b.y)).length();
                            if (QPointF(b.x, b.y) == joined->second)
                                break;
                        }
                        if (added < best_added - 1e-6) {
                            best_added = added;
                            if (reverse)
                                joined->first = joined->first.toReversed();
                            best = std::move(joined);
                        }
                    }
                }
            }
            if (best) {
                item->setPath(best->first);
                item->junctions.push_back(best->second);
            }
        }
        network[endpoint_key(wire->from)].push_back({endpoint_key(wire->to), item->path()});
        network[endpoint_key(wire->to)].push_back({endpoint_key(wire->from), item->path().toReversed()});
        item->update();
    }
    std::erase_if(base_wire_routes_, [&](const auto &entry) { return !wires_.contains(entry.first); });
}
std::string EditorWindow::add_component(Kind kind, QPointF point) {
    if (running())
        return {};
    try {
        point = snapped(point);
        selected_ = document_->add_component(kind, point.x(), point.y());
        refresh();
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
        point = snapped(point);
        selected_ = document_->add_node(ground, point.x(), point.y());
        refresh();
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
        point = snapped(point);
        selected_ = document_->add_pattern(point.x(), point.y());
        refresh();
        return selected_;
    } catch (const std::exception &e) {
        show_error(e);
        return {};
    }
}
std::string EditorWindow::add_plot(QPointF point) {
    if (running())
        return {};
    point = snapped(point);
    selected_ = document_->add_plot(point.x(), point.y(), text("plot").toStdString());
    refresh();
    return selected_;
}
bool EditorWindow::connect_ports(Endpoint from, Endpoint to) {
    if (running())
        return false;
    try {
        document_->connect(std::move(from), std::move(to));
        refresh();
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
    bool moved = false;
    auto orientation = [](QGraphicsItem *item) {
        const auto t = item->transform();
        Orientation o;
        o.mirrored = t.determinant() < 0;
        o.quarter_turns =
            (static_cast<int>(std::lround(std::atan2(t.m12(), t.m22()) / (std::acos(-1.0) / 2))) + 4) % 4;
        return o;
    };
    auto check = [&](const auto &a) {
        if (atoms_.at(a.id)->pos() != QPointF(a.x, a.y) || orientation(atoms_.at(a.id)) != a.orientation)
            moved = true;
    };
    for (const auto &c : project().components)
        check(c);
    for (const auto &n : project().nodes)
        check(n);
    for (const auto &g : project().patterns)
        check(g);
    for (const auto &g : project().plots)
        check(g);
    for (const auto &i : project().instances)
        check(i);
    if (!moved)
        return;
    document_->apply("Move objects", [&](Project &p) {
        for (auto &w : p.wires)
            for (auto &b : w.bends)
                b = canvas_->moving_point(w.from.object, w.to.object, b);
        auto move = [&](auto &a) {
            auto pos = atoms_.at(a.id)->pos();
            a.x = pos.x();
            a.y = pos.y();
            a.orientation = orientation(atoms_.at(a.id));
        };
        for (auto &c : p.components)
            move(c);
        for (auto &n : p.nodes)
            move(n);
        for (auto &g : p.patterns)
            move(g);
        for (auto &g : p.plots)
            move(g);
        for (auto &i : p.instances)
            move(i);
    });
    refresh();
}
void EditorWindow::delete_selected() {
    canvas_->cancel_gesture();
    if (running())
        return;
    std::vector<std::string> ids;
    for (auto *item : canvas_->scene()->selectedItems())
        if (item->data(1).toString() != "label")
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
                        refresh();
                    } catch (const std::exception &e) {
                        show_error(e);
                    }
                    return;
                }
            }
    try {
        document_->erase(ids);
        selected_.clear();
        refresh();
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
void EditorWindow::show_error(const std::exception &e) {
    QString message = QString::fromUtf8(e.what());
    auto *d = dynamic_cast<const Diagnostic *>(&e);
    if (d)
        message = q(d->code) + ": " + message;
    auto *item = new QListWidgetItem(message, errors_);
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
        }
        return;
    }
    simulation_timer_.start();
    if (!commit_inline_edit())
        return;
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
        clear_result();
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
        simulated_time_ = 0;
        preparing_ = true;

        canvas_->set_editable(false);
        library_->setEnabled(false);
        run_->setEnabled(true);
        undo_->setEnabled(false);
        redo_->setEnabled(false);
        banner_->setText(text("preparing") + " · " + text("elapsed").arg(0., 0, 'f', 2));
        busy_ = true;
        update_run_button();
        update_command_state();
        const auto snapshot = root_project();
        const auto keys = recording_keys();
        watcher_.setFuture(QtConcurrent::run([this, snapshot, keys] {
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
                outcome.result =
                    execute(ir, &cancel_, &simulated_time_, &plan, &paused_, [this](Result &&batch) {
                        std::lock_guard lock(stream_mutex_);
                        stream_queue_.push_back(std::move(batch));
                    });
                outcome.execution_seconds = timer.nsecsElapsed() / 1e9;
            } catch (const Diagnostic &e) {
                outcome.error = q(e.code) + ": " + QString::fromUtf8(e.what());
                outcome.object = e.object;
                outcome.path = e.path;
            } catch (const std::exception &e) {
                outcome.error = QString::fromUtf8(e.what());
            }
            return outcome;
        }));
        update_title();
    } catch (const std::exception &e) {
        show_error(e);
    }
}
void EditorWindow::stop_simulation() {
    canvas_->cancel_gesture();
    if (running()) {
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
    auto outcome = completed.takeResult();
    drain_simulation_stream();
    busy_ = false;
    paused_ = false;
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
        auto *item = new QListWidgetItem(outcome.error, errors_);
        item->setData(Qt::UserRole, q(outcome.object));
        QStringList path;
        for (const auto &step : outcome.path)
            path.push_back(q(step));
        item->setData(Qt::UserRole + 1, path);
        bottom_->setCurrentIndex(0);
        banner_->setText(text("error_hint"));
        return;
    }
    append_simulation_result(std::move(*outcome.result));
    findChild<QLabel *>("diagnostic_status")
        ->setText(text(result_->cancelled ? "cancelled" : "diagnostics_ok") + " · " +
                  QString::number(result_->accepted_steps) + " " + text("steps"));
    choose_channels();
    update_graphs();
    if (scope_)
        scope_->set_live(false);
    for (auto &[id, view] : plot_views_)
        if (view)
            view->set_live(false);
    banner_->setText(text(result_->cancelled ? "cancelled" : "complete") + " · " +
                     QString::number(result_->accepted_steps) + " " + text("steps") + " · " +
                     text("elapsed").arg(simulation_timer_.elapsed() / 1000., 0, 'f', 2) +
                     (result_->samples.empty() ? " · " + text("no_recording") : QString()));
    banner_->setToolTip(text("simulation_timing")
                            .arg(outcome.preparation_seconds, 0, 'f', 3)
                            .arg(outcome.execution_seconds, 0, 'f', 3));
    update_title();
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
    statusBar()->showMessage(QString::number(project().components.size()) + " " + text("components") + " · " +
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
        path = QFileDialog::getSaveFileName(this, text("save"), {}, text("project_filter"));
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
