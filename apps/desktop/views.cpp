#include "apps/desktop/editor.hpp"
#include <QApplication>
#include <QContextMenuEvent>
#include <QFile>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QJsonDocument>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>
static void init_resources() {
    Q_INIT_RESOURCE(translations);
}
namespace pds::desktop {
static QJsonObject strings;
void init_language(const QString &language) {
    init_resources();
    QFile file(":/i18n/" + (language == "en" ? QString("en") : QString("ru")) + ".json");
    file.open(QIODevice::ReadOnly);
    strings = QJsonDocument::fromJson(file.readAll()).object();
}
QString text(const char *key) {
    return strings.value(key).toString(QString::fromLatin1(key));
}
Canvas::Canvas(QWidget *parent) : QGraphicsView(parent) {
    setScene(new QGraphicsScene(this));
    setRenderHint(QPainter::Antialiasing);
    setDragMode(RubberBandDrag);
    setTransformationAnchor(AnchorUnderMouse);
    setViewportUpdateMode(BoundingRectViewportUpdate);
    setObjectName("canvas");
    setMouseTracking(true);
}
void Canvas::set_ghost(QGraphicsItem* item){if(ghost_){scene()->removeItem(ghost_);delete ghost_;}ghost_=item;if(item){item->setOpacity(.42);item->setFlags({});item->setAcceptedMouseButtons(Qt::NoButton);for(auto* child:item->childItems()){child->setData(1,{});child->setAcceptedMouseButtons(Qt::NoButton);}item->setZValue(90);scene()->addItem(item);item->hide();}}
void Canvas::leaveEvent(QEvent* e){if(ghost_)ghost_->hide();QGraphicsView::leaveEvent(e);}
void Canvas::drawBackground(QPainter *painter, const QRectF &rect) {
    painter->fillRect(rect, QColor("#f4f7fb"));
    if (transform().m11() < .3)
        return;
    painter->setPen(QPen(QColor("#d9e2ed"), 0));
    const double grid = 20;
    for (double x = std::floor(rect.left() / grid) * grid; x < rect.right(); x += grid)
        for (double y = std::floor(rect.top() / grid) * grid; y < rect.bottom(); y += grid)
            painter->drawPoint(QPointF(x, y));
}
void Canvas::drawForeground(QPainter *painter, const QRectF &) {
    if (!scene()->items().empty())
        return;
    painter->save();
    painter->resetTransform();
    QRectF box = viewport()->rect();
    auto font = painter->font();
    font.setPointSize(20);
    font.setBold(true);
    painter->setFont(font);
    painter->setPen(QColor("#45607f"));
    painter->drawText(box.adjusted(30, -65, -30, -65), Qt::AlignCenter, text("empty_title"));
    font.setPointSize(11);
    font.setBold(false);
    painter->setFont(font);
    painter->setPen(QColor("#8496ab"));
    painter->drawText(box.adjusted(30, 30, -30, 30), Qt::AlignCenter, text("empty_steps"));
    painter->restore();
}
std::optional<Endpoint> Canvas::port_at(QPoint point) const {
    for(auto* item:items(point))if(item->data(1).toString()=="port")return Endpoint{item->data(0).toString().toStdString(),item->data(2).toString().toStdString()};
    return std::nullopt;
}
void Canvas::cancel_wire(){dragging_wire_.reset();if(wire_preview_){scene()->removeItem(wire_preview_);delete wire_preview_;wire_preview_=nullptr;}viewport()->update();}
void Canvas::mousePressEvent(QMouseEvent* e){
    last_mouse_=e->pos();
    if(e->button()==Qt::MiddleButton){panning_=true;pan_origin_=e->pos();setCursor(Qt::ClosedHandCursor);e->accept();return;}
    if(e->button()==Qt::LeftButton){
        if(ghost_&&place){place(mapToScene(e->pos()));e->accept();return;}
        if(auto endpoint=port_at(e->pos())){
            cancel_wire();dragging_wire_=endpoint;wire_origin_=mapToScene(e->pos());
            for(auto* item:items(e->pos()))if(item->data(1).toString()=="port"){wire_origin_=item->scenePos();break;}
            wire_preview_=scene()->addPath(QPainterPath(wire_origin_),QPen(QColor("#467fe0"),1.6,Qt::DashLine));wire_preview_->setZValue(100);wire_preview_->setAcceptedMouseButtons(Qt::NoButton);e->accept();return;
        }
        if(!itemAt(e->pos())&&place)place(mapToScene(e->pos()));
    }
    QGraphicsView::mousePressEvent(e);
}
void Canvas::mouseMoveEvent(QMouseEvent *e) {
    last_mouse_ = e->pos();
    if(ghost_){auto p=mapToScene(e->pos());ghost_->setPos(std::round(p.x()/20)*20,std::round(p.y()/20)*20);ghost_->show();}
    if(dragging_wire_){
        QPointF end=mapToScene(e->pos());auto target=port_at(e->pos());bool valid=false;
        if(target){for(auto* item:items(e->pos()))if(item->data(1).toString()=="port"){end=item->scenePos();break;}valid=*target!=*dragging_wire_&&(!compatible||compatible(*dragging_wire_,*target));}
        QPainterPath path(wire_origin_);double middle=(wire_origin_.x()+end.x())/2;path.lineTo(middle,wire_origin_.y());path.lineTo(middle,end.y());path.lineTo(end);
        wire_preview_->setPath(path);wire_preview_->setPen(QPen(QColor(target?(valid?"#16a085":"#cb5563"):"#467fe0"),1.6,Qt::DashLine));e->accept();return;
    }
    if (panning_) {
        auto delta = e->pos() - pan_origin_;
        pan_origin_ = e->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        return;
    }
    QGraphicsView::mouseMoveEvent(e);
    if (movement)
        movement();
}
void Canvas::mouseReleaseEvent(QMouseEvent *e) {
    if(e->button()==Qt::LeftButton&&dragging_wire_){auto from=*dragging_wire_;auto to=port_at(e->pos());std::string wire;for(auto* item:items(e->pos()))if(item->data(1).toString()=="wire"){wire=item->data(0).toString().toStdString();break;}auto point=mapToScene(e->pos());cancel_wire();if(viewport()->rect().contains(e->pos())&&(!to||*to!=from)&&wire_dropped)wire_dropped(from,to,point,wire);e->accept();return;}

    if (panning_ && e->button() == Qt::MiddleButton) {
        panning_ = false;
        unsetCursor();
        return;
    }
    QGraphicsView::mouseReleaseEvent(e);
    if (released)
        released();
}
std::optional<Endpoint> Canvas::hovered_port() const {
    return port_at(last_mouse_);
}
void Canvas::mouseDoubleClickEvent(QMouseEvent *e) {
    auto *item = itemAt(e->pos());
    if (item && open_object) {
        open_object(item->data(0).toString().toStdString());
        e->accept();
        return;
    }
    QGraphicsView::mouseDoubleClickEvent(e);
}
void Canvas::contextMenuEvent(QContextMenuEvent* e){auto* item=itemAt(e->pos());if(context_menu)context_menu(item?item->data(0).toString().toStdString():std::string(),e->globalPos());}
void Canvas::wheelEvent(QWheelEvent *e) {
    double factor = e->angleDelta().y() > 0 ? 1.15 : 1 / 1.15;
    if (transform().m11() * factor > .15 && transform().m11() * factor < 5)
        scale(factor, factor);
    e->accept();
}
Scope::Scope(QWidget *parent) : QWidget(parent) {
    setMinimumHeight(180);
    setObjectName("scope");
}
void Scope::set_result(const Result *result, const std::vector<int> &channels, const Project &p) {
    result_ = result && !result->samples.empty() ? result : nullptr;
    channels_ = channels;
    begin = p.scope_begin;
    end = p.scope_end;
    cursor_a = p.cursor_a;
    cursor_b = p.cursor_b;
    if (result_ && end <= begin) {
        begin = 0;
        end = std::max(1e-12, result_->samples.back().time);
    }
    update();
}
void Scope::fit() {
    if (!result_)
        return;
    begin = 0;
    end = std::max(1e-12, result_->samples.back().time);
    if (changed)
        changed(begin, end, cursor_a, cursor_b);
    update();
}
double Scope::sample_value(size_t sample, int channel) const {
    const auto &s = result_->samples.at(sample);
    if (channel < static_cast<int>(s.values.size()))
        return s.values.at(channel);
    return s.gates.at(channel - s.values.size()) ? 1 : 0;
}
void Scope::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor("#ffffff"));
    QRectF plot(58, 48, std::max(1, width() - 84), std::max(1, height() - 96));
    painter.setPen(QColor("#91a2b7"));
    painter.drawRect(plot);
    if (!result_ || channels_.empty()) {
        painter.drawText(plot, Qt::AlignCenter, text("scope_empty"));
        return;
    }
    double low = std::numeric_limits<double>::infinity(), high = -low;
    for (size_t i = 0; i < result_->samples.size(); ++i)
        if (result_->samples[i].time >= begin && result_->samples[i].time <= end)
            for (int channel : channels_) {
                const auto v = sample_value(i, channel);
                low = std::min(low, v);
                high = std::max(high, v);
            }
    if (!std::isfinite(low)) {
        low = -1;
        high = 1;
    }
    double margin = std::max(1e-6, (high - low) * .08);
    if (high == low)
        margin = std::max(.1, std::abs(high) * .1);
    low -= margin;
    high += margin;
    for (int grid = 0; grid <= 4; ++grid) {
        double fraction = grid / 4.0;
        painter.setPen(QColor("#e5ebf3"));
        painter.drawLine(QPointF(plot.left(), plot.top() + fraction * plot.height()),
                         QPointF(plot.right(), plot.top() + fraction * plot.height()));
        painter.setPen(QColor("#5f7187"));
        painter.drawText(QRectF(0, plot.top() + fraction * plot.height() - 9, 51, 20), Qt::AlignRight,
                         QString::number(high - fraction * (high - low), 'g', 4));
        painter.drawText(QRectF(plot.left() + fraction * plot.width() - 38, plot.bottom() + 5, 76, 20),
                         Qt::AlignCenter, QString::number((begin + fraction * (end - begin)) * 1000, 'g', 4));
    }
    painter.drawText(QRectF(plot.left(), height() - 22, plot.width(), 18), Qt::AlignCenter, text("time_ms"));
    const QList<QColor> colors = {QColor("#146cca"), QColor("#c56819"), QColor("#17866d"), QColor("#935ad5"),
                                  QColor("#d04769")};
    painter.save();
    painter.setClipRect(plot);
    auto x = [&](double t) { return plot.left() + (t - begin) / (end - begin) * plot.width(); };
    for (size_t ch = 0; ch < channels_.size(); ++ch) {
        QPainterPath path;
        bool started = false;
        // Preserve extrema in each display bucket; the CSV always contains every sample.
        const auto &samples = result_->samples;
        auto first = std::lower_bound(samples.begin(), samples.end(), begin,
                                      [](const Sample &s, double t) { return s.time < t; });
        size_t from = static_cast<size_t>(first - samples.begin());
        if (from)
            --from;
        auto last = std::upper_bound(samples.begin(), samples.end(), end,
                                     [](double t, const Sample &s) { return t < s.time; });
        size_t to = std::min(samples.size(), static_cast<size_t>(last - samples.begin()) + 1);
        const size_t stride =
            std::max<size_t>(1, (to - from) / static_cast<size_t>(std::max(1, width() * 2)));
        bool digital = channels_[ch] >= static_cast<int>(result_->channels.size());
        for (size_t i = from; i < to; i += stride) {
            size_t finish = std::min(i + stride, to), imin = i, imax = i;
            for (size_t k = i + 1; k < finish; ++k) {
                if (sample_value(k, channels_[ch]) < sample_value(imin, channels_[ch]))
                    imin = k;
                if (sample_value(k, channels_[ch]) > sample_value(imax, channels_[ch]))
                    imax = k;
            }
            std::vector<size_t> indices{i, imin, imax, finish - 1};
            std::sort(indices.begin(), indices.end());
            indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
            for (size_t k : indices) {
                double t = samples[k].time, v = sample_value(k, channels_[ch]);
                QPointF point(x(t), plot.bottom() - (v - low) / (high - low) * plot.height());
                if (!started) {
                    path.moveTo(point);
                    started = true;
                } else {
                    if (digital)
                        path.lineTo(point.x(), path.currentPosition().y());
                    path.lineTo(point);
                }
            }
        }
        painter.setPen(QPen(colors[static_cast<int>(ch) % colors.size()], 1.8));
        painter.drawPath(path);
    }
    for (double cursor : {cursor_a, cursor_b})
        if (cursor >= begin && cursor <= end) {
            painter.setPen(QPen(QColor("#334458"), 1, Qt::DashLine));
            painter.drawLine(QPointF(x(cursor), plot.top()), QPointF(x(cursor), plot.bottom()));
        }
    painter.restore();
    auto cursor_label = [&](double cursor, const QString &label, int row) {
        if (cursor < 0)
            return;
        auto it = std::lower_bound(result_->samples.begin(), result_->samples.end(), cursor,
                                   [](const Sample &s, double t) { return s.time < t; });
        size_t index = it == result_->samples.end() ? result_->samples.size() - 1
                                                    : static_cast<size_t>(it - result_->samples.begin());
        if (index && std::abs(result_->samples[index - 1].time - cursor) <
                         std::abs(result_->samples[index].time - cursor))
            --index;
        QString values = label + ": " + QString::number(result_->samples[index].time * 1000, 'g', 6) + " ms";
        for (int ch : channels_) {
            QString unit = ch < static_cast<int>(result_->channels.size())
                               ? QString::fromStdString(result_->channels[ch].unit)
                               : "gate";
            values += "   " + QString::number(sample_value(index, ch), 'g', 6) + " " + unit;
        }
        if (row == 1 && cursor_a >= 0)
            values += "   Δt=" + QString::number((cursor_b - cursor_a) * 1000, 'g', 6) + " ms";
        painter.setPen(QColor("#253950"));
        painter.drawText(QRectF(58, row * 20 + 2, width() - 84, 20), Qt::AlignLeft | Qt::AlignVCenter,
                         values);
    };
    cursor_label(cursor_a, "A", 0);
    cursor_label(cursor_b, "B", 1);
}
void Scope::mousePressEvent(QMouseEvent *e) {
    if (!result_)
        return;
    double t =
        begin + std::clamp((e->position().x() - 58) / std::max(1, width() - 84), 0.0, 1.0) * (end - begin);
    if (e->button() == Qt::RightButton)
        cursor_b = t;
    else
        cursor_a = t;
    if (changed)
        changed(begin, end, cursor_a, cursor_b);
    update();
}
void Scope::wheelEvent(QWheelEvent *e) {
    if (!result_)
        return;
    double fraction = std::clamp((e->position().x() - 58) / std::max(1, width() - 84), 0.0, 1.0);
    double center = begin + fraction * (end - begin),
           range = (end - begin) * (e->angleDelta().y() > 0 ? .8 : 1.25);
    range = std::clamp(range, 1e-12, std::max(1e-12, result_->samples.back().time) * 10);
    begin = std::max(0.0, center - fraction * range);
    end = begin + range;
    if (changed)
        changed(begin, end, cursor_a, cursor_b);
    update();
    e->accept();
}
} // namespace pds::desktop
