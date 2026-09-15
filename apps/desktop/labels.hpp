#pragma once
#include <QFont>
#include <QGraphicsItem>
namespace pds::desktop {
class LabelItem final : public QGraphicsItem {
  public:
    LabelItem(const QString &owner, const QString &role, QGraphicsItem *parent = nullptr);
    void set_text(const QString &text);
    QRectF boundingRect() const override;
    void paint(QPainter *, const QStyleOptionGraphicsItem *, QWidget *) override;

  private:
    QString text_;
    QFont font_;
    QRectF bounds_;
};
} // namespace pds::desktop
