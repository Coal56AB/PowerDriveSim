#include "apps/desktop/editor.hpp"
#include <QAction>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QSettings>
#include <QToolBar>
#include <QTreeWidget>
#include <algorithm>

namespace pds::desktop {
namespace {
QIcon component_icon(int id) {
    QPixmap image(64, 64);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(2, 2);
    p.setPen(QPen(QColor("#294c70"), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    if (id == 100) {
        p.drawLine(16, 4, 16, 17);
        p.drawLine(5, 17, 27, 17);
        p.drawLine(9, 22, 23, 22);
        p.drawLine(13, 27, 19, 27);
    } else if (id == 103) {
        p.drawRoundedRect(QRectF(3, 4, 26, 24), 3, 3);
        p.setPen(QPen(QColor("#5f65d9"), 1.8));
        QPainterPath line(QPointF(6, 22));
        line.cubicTo(13, 22, 12, 8, 19, 12);
        line.cubicTo(23, 15, 23, 21, 27, 14);
        p.drawPath(line);
    } else if (id == 7 || id == 8 || id == 3 || id == 4) {
        p.drawLine(1, 16, 6, 16);
        p.drawEllipse(QRectF(6, 6, 20, 20));
        p.drawLine(26, 16, 31, 16);
        auto f = p.font();
        f.setPixelSize(12);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRectF(6, 6, 20, 20), Qt::AlignCenter, id == 7 || id == 3 ? "V" : "A");
        if (id == 7 || id == 8) {
            p.setPen(QPen(QColor("#8868b8"), 1.6));
            p.drawLine(16, 26, 16, 31);
        }
    } else if (id == 0) {
        p.drawLine(1, 16, 7, 16);
        p.drawRect(QRectF(7, 11, 18, 10));
        p.drawLine(25, 16, 31, 16);
    } else if (id == 1) {
        p.drawLine(1, 16, 12, 16);
        p.drawLine(12, 6, 12, 26);
        p.drawLine(20, 6, 20, 26);
        p.drawLine(20, 16, 31, 16);
    } else if (id == 2) {
        QPainterPath line(QPointF(1, 22));
        for (int i = 0; i < 4; ++i)
            line.cubicTo(2 + i * 7, 7, 8 + i * 7, 7, 8 + i * 7, 22);
        p.drawPath(line);
    } else if (id == 5) {
        p.drawLine(1, 22, 8, 22);
        p.drawLine(8, 22, 24, 12);
        p.drawLine(24, 22, 31, 22);
        p.drawLine(16, 4, 16, 11);
    } else if (id == 6) {
        p.drawLine(1, 16, 8, 16);
        QPolygonF triangle;
        triangle << QPointF(8, 7) << QPointF(24, 16) << QPointF(8, 25);
        p.drawPolygon(triangle);
        p.drawLine(24, 7, 24, 25);
        p.drawLine(24, 16, 31, 16);
    } else {
        p.setPen(QPen(QColor("#17866d"), 1.8));
        QPainterPath line(QPointF(2, 25));
        line.lineTo(6, 25);
        line.lineTo(6, 7);
        line.lineTo(17, 7);
        line.lineTo(17, 25);
        line.lineTo(26, 25);
        line.lineTo(26, 7);
        line.lineTo(30, 7);
        p.drawPath(line);
    }
    return QIcon(image);
}
} // namespace
void EditorWindow::begin_placement(int id) {
    if (running())
        return;
    canvas_->cancel_gesture();
    placing_ = id;
    set_placement_preview();
    banner_->setText(text("place_hint"));
    canvas_->setFocus();
}
void EditorWindow::build_component_palette(QLineEdit *search) {
    std::map<std::string, QTreeWidgetItem *> categories;
    std::vector<QJsonObject> ordered;
    for (const auto &[type, spec] : component_specs_)
        if (spec.contains("palette"))
            ordered.push_back(spec);
    std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
        return a.value("palette").toObject().value("id").toInt() <
               b.value("palette").toObject().value("id").toInt();
    });
    for (const auto &spec : ordered) {
        const auto entry = spec.value("palette").toObject();
        if (entry.isEmpty() || entry.value("hidden").toBool())
            continue;
        const int id = entry.value("id").toInt();
        const auto label = entry.value("label").toString().toUtf8();
        const auto group = entry.value("category").toString().toUtf8();
        const auto short_name = entry.value("short").toString();
        auto *action = new QAction(text(label.constData()), this);
        action->setObjectName("insert_component_" + QString::number(id));
        action->setData(id);
        action->setProperty("fixed", entry.value("fixed").toBool());
        action->setIconText(short_name);
        action->setIcon(component_icon(id));
        action->setToolTip(text(label.constData()));
        connect(action, &QAction::triggered, this, [this, id = id] { begin_placement(id); });
        component_actions_[id] = action;
        if (group.isEmpty())
            continue;
        auto *&category = categories[group.toStdString()];
        if (!category) {
            category = new QTreeWidgetItem(library_, {text(group.constData())});
            auto font = category->font(0);
            font.setBold(true);
            category->setFont(0, font);
            category->setFlags(Qt::ItemIsEnabled);
            category->setExpanded(false);
        }
        auto *item = new QTreeWidgetItem(category, {text(label.constData())});
        item->setData(0, Qt::UserRole, id);
    }
    search->setObjectName("library_search");
    connect(
        search, &QLineEdit::textChanged, this,
        [this, filtering = false, expanded = std::set<QTreeWidgetItem *>{}](const QString &query) mutable {
            const bool active = !query.trimmed().isEmpty();
            if (active && !filtering) {
                expanded.clear();
                for (int i = 0; i < library_->topLevelItemCount(); ++i)
                    if (library_->topLevelItem(i)->isExpanded())
                        expanded.insert(library_->topLevelItem(i));
            }
            for (int i = 0; i < library_->topLevelItemCount(); ++i) {
                auto *parent = library_->topLevelItem(i);
                bool any = false;
                for (int j = 0; j < parent->childCount(); ++j) {
                    auto *child = parent->child(j);
                    const bool match = !active || child->text(0).contains(query, Qt::CaseInsensitive) ||
                                       parent->text(0).contains(query, Qt::CaseInsensitive);
                    child->setHidden(!match);
                    any |= match;
                }
                parent->setHidden(!any);
                if (active || filtering)
                    parent->setExpanded(active || expanded.count(parent));
            }
            filtering = active;
        });
    connect(library_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (item->data(0, Qt::UserRole).isValid())
            begin_placement(item->data(0, Qt::UserRole).toInt());
    });
    library_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(library_, &QWidget::customContextMenuRequested, this, [this](QPoint point) {
        auto *item = library_->itemAt(point);
        if (!item || !item->data(0, Qt::UserRole).isValid())
            return;
        int id = item->data(0, Qt::UserRole).toInt();
        bool pinned =
            std::find(pinned_components_.begin(), pinned_components_.end(), id) != pinned_components_.end();
        QMenu menu(this);
        menu.setObjectName("component_pin_menu");
        menu.addAction(text(pinned ? "unpin_component" : "pin_component"), this,
                       [this, id, pinned] { set_component_pinned(id, !pinned); });
        menu.exec(library_->viewport()->mapToGlobal(point));
    });
    addToolBarBreak(Qt::TopToolBarArea);
    component_bar_ = addToolBar(text("component_bar"));
    component_bar_->setObjectName("component_bar");
    component_bar_->setMovable(false);
    component_bar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    component_bar_->setIconSize(QSize(28, 28));
    component_bar_->setStyleSheet(
        "QToolBar{background:#f4f7fb;border-bottom:1px solid #dfe6ef;spacing:5px;padding:4px;}"
        "QToolButton{color:#253e5c;background:white;border:1px solid #d4dfed;border-radius:4px;padding:5px "
        "12px;}"
        "QToolButton:hover{background:#e7f0ff;} QToolButton:disabled{color:#9aa6b5;}");
    component_bar_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(component_bar_, &QWidget::customContextMenuRequested, this, [this](QPoint point) {
        auto *action = component_bar_->actionAt(point);
        if (!action || !action->data().isValid() || action->property("fixed").toBool())
            return;
        const int id = action->data().toInt();
        QMenu menu(this);
        menu.setObjectName("component_unpin_menu");
        menu.addAction(text("unpin_component"), this, [this, id] { set_component_pinned(id, false); });
        menu.exec(component_bar_->mapToGlobal(point));
    });
    QSettings settings(recovery_dir_ + "/ui.ini", QSettings::IniFormat);
    for (const auto &value : settings.value("pinned_components").toStringList()) {
        bool ok = false;
        const int id = value.toInt(&ok);
        if (ok && component_actions_.count(id) && !component_actions_.at(id)->property("fixed").toBool() &&
            std::find(pinned_components_.begin(), pinned_components_.end(), id) == pinned_components_.end())
            pinned_components_.push_back(id);
    }
    rebuild_component_bar();
}
void EditorWindow::rebuild_component_bar() {
    for (auto *action : component_bar_->actions())
        if (action->isSeparator())
            delete action;
    component_bar_->clear();
    for (int id : {100, 7, 8, 103})
        if (component_actions_.count(id) && component_actions_.at(id)->property("fixed").toBool())
            component_bar_->addAction(component_actions_.at(id));
    for (const auto &[id, action] : component_actions_)
        if (action->property("fixed").toBool() && !component_bar_->actions().contains(action))
            component_bar_->addAction(action);
    if (!pinned_components_.empty())
        component_bar_->addSeparator();
    for (int id : pinned_components_)
        component_bar_->addAction(component_actions_.at(id));
    component_bar_->setToolTip(text("component_bar_hint"));
}
void EditorWindow::set_component_pinned(int id, bool pinned) {
    if (!component_actions_.count(id) || component_actions_.at(id)->property("fixed").toBool())
        return;
    auto found = std::find(pinned_components_.begin(), pinned_components_.end(), id);
    if (pinned && found == pinned_components_.end())
        pinned_components_.push_back(id);
    else if (!pinned && found != pinned_components_.end())
        pinned_components_.erase(found);
    QDir().mkpath(recovery_dir_);
    QSettings settings(recovery_dir_ + "/ui.ini", QSettings::IniFormat);
    QStringList ids;
    for (int value : pinned_components_)
        ids.push_back(QString::number(value));
    settings.setValue("pinned_components", ids);
    settings.sync();
    rebuild_component_bar();
}
} // namespace pds::desktop
