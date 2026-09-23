#include "apps/desktop/code_icon_editor.hpp"
#include "apps/desktop/theme.hpp"
#include "apps/desktop/editor.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <algorithm>
#include <cmath>

namespace pds::desktop {
namespace {
QColor primitive_color(IconColor color) {
    switch (color) {
    case IconColor::signal:return theme_colors().signal;
    case IconColor::gate:return theme_colors().gate;
    case IconColor::accent:return theme_colors().accent;
    default:return theme_colors().text;
    }
}
QPointF map_point(const Point &point, const QRectF &target) {
    return {target.left()+point.x/32.0*target.width(),target.top()+point.y/32.0*target.height()};
}
}

void paint_code_icon(QPainter &p, const std::vector<IconPrimitive> &icon, const QRectF &target) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setClipRect(target.adjusted(-1,-1,1,1));
    const double scale=std::min(target.width(),target.height())/32.0;
    for(const auto &primitive:icon) {
        const QColor color=primitive_color(primitive.color);
        p.setPen(QPen(color,std::max(.8,1.7*scale),Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        p.setBrush(primitive.filled?QBrush(color):Qt::NoBrush);
        if(primitive.kind==IconPrimitiveKind::text) {
            if(primitive.points.empty())continue;
            auto font=p.font();
            const double size=primitive.points.size()>1?primitive.points[1].x:9;
            font.setPixelSize(std::max(5,int(std::round(size*scale))));font.setBold(true);p.setFont(font);
            const QPointF center=map_point(primitive.points.front(),target);
            p.drawText(QRectF(center.x()-target.width()/2,center.y()-target.height()/2,target.width(),target.height()),
                       Qt::AlignCenter,QString::fromStdString(primitive.text));
            continue;
        }
        if(primitive.points.size()<2)continue;
        const QPointF first=map_point(primitive.points[0],target),second=map_point(primitive.points[1],target);
        if(primitive.kind==IconPrimitiveKind::line)p.drawLine(first,second);
        else if(primitive.kind==IconPrimitiveKind::rectangle)p.drawRect(QRectF(first,second).normalized());
        else if(primitive.kind==IconPrimitiveKind::ellipse)p.drawEllipse(QRectF(first,second).normalized());
        else {
            QPainterPath path(first);
            for(size_t index=1;index<primitive.points.size();++index)path.lineTo(map_point(primitive.points[index],target));
            p.drawPath(path);
        }
    }
    p.restore();
}

CodeIconEditor::CodeIconEditor(const std::vector<IconPrimitive> &icon,QWidget *parent):QWidget(parent),icon_(icon) {
    setMinimumSize(300,260);setFocusPolicy(Qt::StrongFocus);setObjectName("code_icon_canvas");
    auto button=[&](const char *key,Tool tool) {
        auto *item=new QToolButton(this);item->setObjectName(key);item->setText(text(key));item->setCheckable(true);item->setAutoExclusive(true);
        connect(item,&QToolButton::clicked,this,[this,tool]{tool_=tool;selected_=-1;update();});
        return item;
    };
    auto *bar=new QHBoxLayout;bar->setContentsMargins(0,0,0,0);
    auto *select=button("icon_select",Tool::select);select->setChecked(true);
    bar->addWidget(select);bar->addWidget(button("icon_line",Tool::line));
    bar->addWidget(button("icon_rectangle",Tool::rectangle));
    bar->addWidget(button("icon_ellipse",Tool::ellipse));
    bar->addWidget(button("icon_freehand",Tool::polyline));
    bar->addWidget(button("icon_text",Tool::text));
    auto *colors=new QComboBox(this);colors->setObjectName("code_icon_color");
    colors->addItem(text("icon_foreground"),int(IconColor::foreground));
    colors->addItem(text("icon_signal"),int(IconColor::signal));
    colors->addItem(text("icon_gate"),int(IconColor::gate));
    colors->addItem(text("icon_accent"),int(IconColor::accent));bar->addWidget(colors);
    auto *fill=new QCheckBox(text("icon_fill"),this);bar->addWidget(fill);
    auto *snap=new QCheckBox(text("icon_snap"),this);snap->setChecked(true);snap->setObjectName("code_icon_snap");bar->addWidget(snap);
    auto *grid=new QSpinBox(this);grid->setRange(1,16);grid->setValue(4);grid->setSuffix(" / 32");
    grid->setToolTip(text("icon_grid_step"));grid->setObjectName("code_icon_grid_step");bar->addWidget(grid);
    auto *remove=new QPushButton(text("delete"),this);remove->setObjectName("delete_icon_primitive");bar->addWidget(remove);
    auto *clear=new QPushButton(text("icon_clear"),this);clear->setObjectName("clear_code_icon");bar->addWidget(clear);
    auto *layout=new QVBoxLayout(this);layout->addLayout(bar);layout->addStretch();
    connect(colors,&QComboBox::currentIndexChanged,this,[this,colors]{color_=IconColor(colors->currentData().toInt());});
    connect(fill,&QCheckBox::toggled,this,[this](bool enabled){filled_=enabled;});
    connect(snap,&QCheckBox::toggled,this,[this](bool enabled){snap_to_grid_=enabled;update();});
    connect(grid,&QSpinBox::valueChanged,this,[this](int step){grid_step_=step;update();});
    connect(remove,&QPushButton::clicked,this,[this]{remove_selected();});
    connect(clear,&QPushButton::clicked,this,[this]{icon_.clear();selected_=-1;update();});
}
std::vector<IconPrimitive> CodeIconEditor::icon() const{return icon_;}
void CodeIconEditor::set_icon(std::vector<IconPrimitive> icon){icon_=std::move(icon);selected_=-1;update();}
QRectF CodeIconEditor::drawing_rect() const {
    const double side=std::max(32.0,std::min(width()-24.0,height()-72.0));
    return {(width()-side)/2.0,54.0+(height()-54.0-side)/2.0,side,side};
}
QPointF CodeIconEditor::to_icon(QPointF point) const {
    const auto rect=drawing_rect();
    QPointF result{std::clamp((point.x()-rect.left())/rect.width()*32.0,0.0,32.0),
                   std::clamp((point.y()-rect.top())/rect.height()*32.0,0.0,32.0)};
    if(snap_to_grid_) {
        result.setX(std::clamp(std::round(result.x()/grid_step_)*grid_step_,0.0,32.0));
        result.setY(std::clamp(std::round(result.y()/grid_step_)*grid_step_,0.0,32.0));
    }
    return result;
}
QPointF CodeIconEditor::from_icon(const Point &point) const{return map_point(point,drawing_rect());}
void CodeIconEditor::paintEvent(QPaintEvent *) {
    QPainter p(this);p.setRenderHint(QPainter::Antialiasing);const auto rect=drawing_rect();
    p.fillRect(rect,theme_colors().canvas);p.setPen(QPen(theme_colors().grid,1));
    for(double value=0;value<=32+1e-9;value+=grid_step_){double x=rect.left()+rect.width()*value/32.0,y=rect.top()+rect.height()*value/32.0;p.drawLine(QPointF(x,rect.top()),QPointF(x,rect.bottom()));p.drawLine(QPointF(rect.left(),y),QPointF(rect.right(),y));}
    p.setPen(QPen(theme_colors().border,1.5));p.drawRect(rect);paint_code_icon(p,icon_,rect);
    if(selected_>=0&&selected_<int(icon_.size())) {
        const auto &item=icon_[size_t(selected_)];QRectF bounds;
        for(const auto &point:item.points)bounds|=QRectF(from_icon(point),QSizeF(1,1));
        p.setPen(QPen(theme_colors().selected,2,Qt::DashLine));p.setBrush(Qt::NoBrush);p.drawRect(bounds.adjusted(-5,-5,5,5));
    }
    if(dragging_) {
        p.setPen(QPen(primitive_color(color_),3,Qt::DashLine,Qt::RoundCap,Qt::RoundJoin));p.setBrush(Qt::NoBrush);
        const auto snapped_start=from_icon({to_icon(start_).x(),to_icon(start_).y()});
        const auto snapped_current=from_icon({to_icon(current_).x(),to_icon(current_).y()});
        if(tool_==Tool::line)p.drawLine(snapped_start,snapped_current);
        else if(tool_==Tool::rectangle)p.drawRect(QRectF(snapped_start,snapped_current).normalized());
        else if(tool_==Tool::ellipse)p.drawEllipse(QRectF(snapped_start,snapped_current).normalized());
        else if(tool_==Tool::polyline&&!stroke_.empty()) {
            QPainterPath path(from_icon(stroke_.front()));
            for(size_t index=1;index<stroke_.size();++index)path.lineTo(from_icon(stroke_[index]));
            path.lineTo(snapped_current);p.drawPath(path);
        }
    }
}
void CodeIconEditor::mousePressEvent(QMouseEvent *event) {
    if(!drawing_rect().contains(event->position()))return;
    setFocus();
    if(tool_==Tool::select) {
        selected_=-1;double best=18;
        for(size_t i=0;i<icon_.size();++i)for(const auto &point:icon_[i].points) {
            double distance=QLineF(event->position(),from_icon(point)).length();if(distance<best){best=distance;selected_=int(i);}
        }
        update();return;
    }
    if(tool_==Tool::text) {
        if(icon_.size()>=max_icon_primitives)return;
        bool ok=false;const QString value=QInputDialog::getText(this,text("icon_text"),text("icon_text_prompt"),QLineEdit::Normal,{},&ok);
        if(ok&&!value.trimmed().isEmpty()) {
            const auto point=to_icon(event->position());
            icon_.push_back({IconPrimitiveKind::text,color_,false,value.trimmed().toStdString(),{{point.x(),point.y()},{9,0}}});
            selected_=int(icon_.size())-1;update();
        }
        return;
    }
    dragging_=true;start_=current_=event->position();stroke_.clear();
    if(tool_==Tool::polyline){auto point=to_icon(start_);stroke_.push_back({point.x(),point.y()});}
}
void CodeIconEditor::mouseMoveEvent(QMouseEvent *event) {
    if(!dragging_)return;current_=event->position();
    if(tool_==Tool::polyline&&stroke_.size()<max_icon_points){auto point=to_icon(current_);if(stroke_.empty()||std::hypot(point.x()-stroke_.back().x,point.y()-stroke_.back().y)>.5)stroke_.push_back({point.x(),point.y()});}
    update();
}
void CodeIconEditor::mouseReleaseEvent(QMouseEvent *event) {
    if(!dragging_)return;dragging_=false;current_=event->position();
    IconPrimitive primitive;primitive.color=color_;primitive.filled=filled_;
    const auto first=to_icon(start_),last=to_icon(current_);
    if(tool_==Tool::polyline){primitive.kind=IconPrimitiveKind::polyline;primitive.points=stroke_;}
    else {primitive.kind=tool_==Tool::line?IconPrimitiveKind::line:tool_==Tool::rectangle?IconPrimitiveKind::rectangle:IconPrimitiveKind::ellipse;primitive.points={{first.x(),first.y()},{last.x(),last.y()}};}
    if(icon_.size()<max_icon_primitives&&primitive.points.size()>=2&&
       QLineF(from_icon(primitive.points.front()),from_icon(primitive.points.back())).length()>2) {
        icon_.push_back(std::move(primitive));selected_=int(icon_.size())-1;
    }
    update();
}
void CodeIconEditor::remove_selected(){if(selected_>=0&&selected_<int(icon_.size()))icon_.erase(icon_.begin()+selected_);selected_=-1;update();}
void CodeIconEditor::keyPressEvent(QKeyEvent *event){if(event->key()==Qt::Key_Delete||event->key()==Qt::Key_Backspace)remove_selected();else QWidget::keyPressEvent(event);}
} // namespace pds::desktop
