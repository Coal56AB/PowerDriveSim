#include "apps/desktop/labels.hpp"
#include <QApplication>
#include <QFontMetricsF>
#include <QPainter>
#include <algorithm>
namespace pds::desktop {
LabelItem::LabelItem(const QString &owner, const QString &role, QGraphicsItem *parent)
    : QGraphicsItem(parent), font_(QApplication::font()) {
    setData(0, owner);
    setData(1, "label");
    setData(2, role);
    setFlags(ItemIsSelectable);
    setZValue(4);
    setCursor(Qt::SizeAllCursor);
}
void LabelItem::set_text(const QString &text) {
    if (text_ == text)
        return;
    prepareGeometryChange();
    text_ = text;
    QFontMetricsF m(font_);
    const double w = std::max(12., m.horizontalAdvance(text) + 8), h = m.height() + 4;
    bounds_ = {-w / 2, -h / 2, w, h};
    update();
}
QRectF LabelItem::boundingRect() const {
    return bounds_;
}
void LabelItem::paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) {
    p->setFont(font_);
    p->setPen(isSelected() ? QColor("#d7811c") : QColor("#263c55"));
    p->drawText(bounds_, Qt::AlignCenter, text_);
    if (isSelected()) {
        p->setPen(QPen(QColor("#db923d"), 1, Qt::DashLine));
        p->setBrush(QColor(255, 194, 87, 18));
        p->drawRect(bounds_);
    }
}
} // namespace pds::desktop
