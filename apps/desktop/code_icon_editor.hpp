#pragma once

#include "core/model/model.hpp"
#include <QWidget>

class QPainter;
class QRectF;

namespace pds::desktop {

void paint_code_icon(QPainter &painter, const std::vector<IconPrimitive> &icon, const QRectF &target);

class CodeIconEditor final : public QWidget {
  public:
    explicit CodeIconEditor(const std::vector<IconPrimitive> &icon, QWidget *parent = nullptr);
    std::vector<IconPrimitive> icon() const;
    void set_icon(std::vector<IconPrimitive> icon);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;

  private:
    enum class Tool { select, line, rectangle, ellipse, polyline, text };
    std::vector<IconPrimitive> icon_;
    Tool tool_ = Tool::select;
    IconColor color_ = IconColor::foreground;
    bool filled_ = false, dragging_ = false, snap_to_grid_ = true;
    double grid_step_ = 4;
    int selected_ = -1;
    QPointF start_, current_;
    std::vector<Point> stroke_;
    QRectF drawing_rect() const;
    QPointF to_icon(QPointF point) const;
    QPointF from_icon(const Point &point) const;
    void remove_selected();
};

} // namespace pds::desktop
