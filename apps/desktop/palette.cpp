#include "apps/desktop/editor.hpp"
#include "apps/desktop/theme.hpp"
#include "formats/project/project.hpp"
#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QSettings>
#include <QHeaderView>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <algorithm>
#include <sstream>

namespace pds::desktop {
namespace {
std::map<std::string, int> definition_icons;
}
int definition_icon_id(const std::string &id) {
    if (id == "f27aab68-ffd2-5a45-a0ec-b2fcaf338e70")
        return 250;
    if (id == "a4c4e197-11d8-5be1-9d41-71d772ca9ac1")
        return 251;
    if (id == "1a963f2c-ceb8-5cce-b927-44d735ec9e80")
        return 280;
    if (id == "eb613164-faf4-5b03-9014-806885fef344")
        return 281;
    if (id == "74a617da-88f1-5a89-bec5-a6a7f92b1000")
        return 282;
    if (id == "7947b68c-41de-59de-aa7e-fe70a3849762")
        return 260;
    if (id == "8cdac591-b468-5d74-8b29-c4c34714c7a0")
        return 261;
    if (id == "d745e2d9-4d37-5c64-97d7-45d87d506cb8")
        return 270;
    const auto found = definition_icons.find(id);
    return found == definition_icons.end() ? -1 : found->second;
}
QIcon component_icon(int id) {
    QPixmap image(128, 128);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(4, 4);
    p.setPen(QPen(theme_colors().text, 0.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    auto title = [&](const QString &s) {
        p.setPen(QPen(theme_colors().text, 0.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        auto f = p.font();
        f.setPixelSize(6);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRectF(1, 25, 30, 7), Qt::AlignCenter, s);
    };
    auto transistor_mark = [&](double x, double y) {
        // Compact IGBT with antiparallel diode.  Composite-library icons are only
        // 32 logical pixels wide, so a single clean device mark is more legible
        // than several full-size marks superimposed on one another.
        p.drawLine(QPointF(x - 9, y), QPointF(x - 5, y));
        p.drawLine(QPointF(x - 5, y - 7), QPointF(x - 5, y + 7));
        p.drawLine(QPointF(x - 1, y - 6), QPointF(x - 1, y + 6));
        p.drawLine(QPointF(x - 1, y - 4), QPointF(x + 5, y - 8));
        p.drawLine(QPointF(x - 1, y + 4), QPointF(x + 5, y + 8));
        p.drawLine(QPointF(x + 5, y - 8), QPointF(x + 5, y + 8));
        QPolygonF diode;
        diode << QPointF(x + 9, y + 4) << QPointF(x + 13, y) << QPointF(x + 9, y - 4);
        p.drawPolyline(diode);
        p.drawLine(QPointF(x + 13, y - 4), QPointF(x + 13, y + 4));
    };
    auto thyristor_mark = [&](double x, double y) {
        p.drawLine(QPointF(x - 13, y), QPointF(x - 7, y));
        p.drawLine(QPointF(x + 8, y), QPointF(x + 14, y));
        QPolygonF tri;
        tri << QPointF(x - 7, y - 8) << QPointF(x - 7, y + 8) << QPointF(x + 7, y);
        p.drawPolygon(tri);
        p.drawLine(QPointF(x + 8, y - 8), QPointF(x + 8, y + 8));
        p.setPen(QPen(theme_colors().gate, 0.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(QPointF(x, y + 11), QPointF(x + 7, y + 4));
        p.setPen(QPen(theme_colors().text, 0.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    };
    if (id == 100) {
        p.drawLine(16, 4, 16, 17);
        p.drawLine(5, 17, 27, 17);
        p.drawLine(9, 22, 23, 22);
        p.drawLine(13, 27, 19, 27);
    } else if (id == 103) {
        p.drawRoundedRect(QRectF(3, 4, 26, 24), 3, 3);
            p.setPen(QPen(theme_colors().signal, 1.0));
        QPainterPath line(QPointF(6, 22));
        line.cubicTo(13, 22, 12, 8, 19, 12);
        line.cubicTo(23, 15, 23, 21, 27, 14);
        p.drawPath(line);
    } else if (id == 106) {
        p.setPen(QPen(theme_colors().gate, 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath tag;
        tag.moveTo(4, 9);
        tag.lineTo(21, 9);
        tag.lineTo(29, 16);
        tag.lineTo(21, 23);
        tag.lineTo(4, 23);
        tag.closeSubpath();
        p.drawPath(tag);
        p.drawEllipse(QPointF(9, 16), 1.8, 1.8);
        p.drawText(QRectF(12, 10, 12, 12), Qt::AlignCenter, "T");
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
            p.setPen(QPen(theme_colors().signal, 1.0));
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
    } else if (id == 270) {
        p.drawRoundedRect(QRectF(3, 3, 26, 21), 2, 2);
        p.drawLine(0, 8, 3, 8); p.drawLine(29, 8, 32, 8);
        p.drawLine(0, 14, 3, 14); p.drawLine(29, 14, 32, 14);
        p.drawLine(0, 20, 3, 20); p.drawLine(29, 20, 32, 20);
        QPainterPath winding(QPointF(8, 8));
        winding.cubicTo(11, 5, 13, 11, 16, 8);
        winding.cubicTo(19, 5, 21, 11, 24, 8);
        p.drawPath(winding);
        p.drawLine(8, 14, 24, 14);
        p.drawLine(8, 20, 24, 20);
        title("Open-end");
    } else if (id == 260 || id == 261) {
        p.drawRoundedRect(QRectF(3, 2, 26, 22), 2, 2);
        p.drawLine(0, 5, 3, 5);
        p.drawLine(0, 21, 3, 21);
        for (int y : {6, 13, 20}) p.drawLine(29, y, 32, y);
        transistor_mark(14, 13);
        if (id == 261) {
            p.setPen(QPen(theme_colors().signal, 0.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawLine(6, 13, 9, 13);
            p.drawEllipse(QPointF(7.5, 13), 1.1, 1.1);
            p.setPen(QPen(theme_colors().text, 0.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        }
        title(id == 260 ? "2-Level" : "3-Level");
    } else if (id == 250 || id == 251) {
        thyristor_mark(16, 12);
        if (id == 251) {
            p.drawLine(4, 5, 28, 5);
            p.drawLine(4, 19, 28, 19);
        }
        title(id == 251 ? "3-Phase" : "1-Phase");
    } else if (id == 282) {
        p.drawLine(2, 8, 30, 8);
        p.drawLine(2, 16, 30, 16);
        p.drawLine(2, 24, 30, 24);
        for (int y : {8, 16, 24}) {
            p.drawLine(10, y, 20, y - 5);
            p.setPen(QPen(theme_colors().gate, 0.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawLine(14, y + 5, 14, y + 1);
            p.setPen(QPen(theme_colors().text, 0.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        }
    } else if (id >= 240 && id <= 242) {
        if (id == 241) {
            p.drawLine(16, 1, 16, 5); p.drawRect(QRectF(12, 5, 8, 9));
            p.drawLine(16, 14, 16, 19); p.drawLine(6, 19, 26, 19);
            p.drawLine(6, 24, 26, 24); p.drawLine(16, 24, 16, 31);
        } else {
            p.drawLine(1, 22, 6, 22); p.drawRect(QRectF(6, 18, 10, 8));
            p.drawLine(16, 22, 20, 22); p.drawLine(20, 22, 27, 16); p.drawLine(28, 22, 31, 22);
            if (id == 240) {
                p.drawLine(2, 22, 2, 8); p.drawLine(2, 8, 11, 8);
                p.drawLine(11, 8, 21, 2); p.drawLine(23, 8, 30, 8); p.drawLine(30, 8, 30, 22);
            }
        }
    } else if (id == 230 || id == 231) {
        p.drawRoundedRect(QRectF(3, 2, 26, 22), 2, 2);
        p.drawLine(0, 5, 3, 5); p.drawLine(0, 21, 3, 21);
        p.drawLine(29, 9, 32, 9);
        if (id == 231) p.drawLine(29, 17, 32, 17);
        transistor_mark(14, 13);
        title(id == 230 ? "Half" : "Full");
    } else if (id >= 220 && id <= 223) {
        p.drawRoundedRect(QRectF(4, 3, 24, 26), 2, 2);
        p.drawLine(0, 16, 4, 16); p.drawLine(28, 16, 32, 16);
        const int from = id == 220 ? 10 : 22, to = id == 220 ? 22 : 10;
        p.drawLine(9, from, 16, from); p.drawLine(16, from, 16, to);
        p.drawLine(16, to, 23, to);
        p.drawLine(20, to - 3, 23, to); p.drawLine(23, to, 20, to + 3);
        if (id == 222) p.drawLine(7, 8, 12, 8);
        if (id == 223) { p.drawLine(9, from, 12, from - 3); p.drawLine(9, from, 12, from + 3); }
    } else if (id >= 210 && id <= 213) {
        p.drawRoundedRect(QRectF(4, 3, 24, 26), 2, 2);
        p.drawLine(5, 28, 27, 4);
        p.drawLine(0, 12, 4, 12);
        p.drawLine(0, 22, 4, 22);
        if (id == 211 || id == 213) p.drawLine(0, 17, 4, 17);
        p.drawLine(28, 10, 32, 10);
        p.drawLine(28, 24, 32, 24);
        p.drawText(QRectF(5, 4, 13, 13), Qt::AlignCenter, "~");
        p.drawLine(19, 20, 25, 20);
        p.drawLine(19, 23, 25, 23);
        if (id >= 212) {
            p.setPen(QPen(theme_colors().gate, 1.0));
            p.drawLine(12, 32, 12, 25);
        }
    } else if (id == 10 || id == 200 || id == 201) {
        p.drawLine(1, 22, 8, 22);
        p.drawLine(8, 22, 8, 14);
        p.drawLine(24, 14, 24, 22);
        p.drawLine(24, 22, 31, 22);
        if (id == 200) {
            p.drawLine(5, 14, 10, 14);
            p.drawLine(14, 14, 18, 14);
            p.drawLine(22, 14, 27, 14);
        } else
            p.drawLine(5, 14, 27, 14);
        p.drawLine(5, 8, 27, 8);
        p.drawLine(16, 1, 16, 8);
        if (id != 200) {
            p.drawLine(18, 18, 24, 22);
            p.drawLine(24, 22, 18, 22);
        }
        if (id >= 200) {
            p.drawLine(4, 28, 28, 28);
            p.drawLine(4, 28, 4, 22);
            p.drawLine(28, 28, 28, 22);
            p.drawLine(18, 25, 12, 28);
            p.drawLine(12, 28, 18, 31);
            p.drawLine(12, 25, 12, 31);
        }
    } else if (id == 280 || id == 281) {
        p.drawRoundedRect(QRectF(3, 3, 26, 21), 2, 2);
        p.drawLine(0, 14, 3, 14);
        for (int y : {7, 14, 21}) p.drawLine(29, y, 32, y);
        p.drawEllipse(QRectF(7, 5, 18, 18));
        QPainterPath wave(QPointF(9, 14));
        wave.cubicTo(11, 8, 14, 8, 16, 14);
        wave.cubicTo(18, 20, 21, 20, 23, 14);
        p.drawPath(wave);
        p.setPen(QPen(theme_colors().signal, 0.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (id == 280) {
            p.drawLine(16, 26, 16, 23);
            p.drawLine(12, 26, 20, 26);
        } else {
            QPainterPath delta(QPointF(12, 27));
            delta.lineTo(20, 27);
            delta.lineTo(16, 23);
            delta.closeSubpath();
            p.drawPath(delta);
        }
    } else if (id == 5) {
        p.drawLine(1, 22, 8, 22);
        p.drawLine(8, 22, 24, 12);
        p.drawLine(24, 22, 31, 22);
        p.drawLine(16, 4, 16, 11);
    } else if (id == 6 || id == 9) {
        p.drawLine(1, 16, 8, 16);
        QPolygonF triangle;
        triangle << QPointF(8, 7) << QPointF(24, 16) << QPointF(8, 25);
        p.drawPolygon(triangle);
        p.drawLine(24, 7, 24, 25);
        p.drawLine(24, 16, 31, 16);
        if (id == 9) {
            p.drawLine(16, 1, 16, 4);
            p.drawLine(16, 4, 24, 11);
        }
    } else {
        p.setPen(QPen(theme_colors().gate, 1.0));
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
void EditorWindow::refresh_component_icons() {
    for (const auto &[id, action] : component_actions_)
        action->setIcon(component_icon(id));
}
void EditorWindow::begin_placement(int id) {
    if (running())
        return;
    canvas_->cancel_gesture();
    const auto action = component_actions_.find(id);
    if (action != component_actions_.end()) {
        const auto path = action->second->property("template").toString();
        if (!path.isEmpty()) {
            try {
                const auto local = QCoreApplication::applicationDirPath() + "/library/" + path;
                QFile file(QFile::exists(local) ? local : ":/library/" + path);
                if (!file.open(QIODevice::ReadOnly))
                    throw std::runtime_error(file.errorString().toStdString());
                std::istringstream input(file.readAll().toStdString());
                paste_fragment_ = read_project(input);
            } catch (const std::exception &e) {
                show_error(e);
                return;
            }
        }
    }
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
        if (entry.isEmpty())
            continue;
        const int id = entry.value("id").toInt();
        const auto label = entry.value("label").toString().toUtf8();
        const auto group = entry.value("category").toString().toUtf8();
        const auto short_name = entry.value("short").toString();
        auto *action = new QAction(text(label.constData()), this);
        action->setObjectName("insert_component_" + QString::number(id));
        action->setData(id);
        action->setProperty("fixed", entry.value("fixed").toBool());
        action->setProperty("template", spec.value("template").toString());
        action->setIconText(short_name);
        action->setIcon(component_icon(id));
        action->setToolTip(text(label.constData()));
        connect(action, &QAction::triggered, this, [this, id = id] { begin_placement(id); });
        component_actions_[id] = action;
        if (spec.contains("template")) {
            QFile file(":/library/" + spec.value("template").toString());
            if (file.open(QIODevice::ReadOnly)) {
                const auto template_path = spec.value("template").toString();
                std::istringstream input(file.readAll().toStdString());
                Project project;
                try {
                    project = read_project(input);
                } catch (const std::exception &e) {
                    throw std::runtime_error(("Failed to read library template " + template_path + ": " +
                                              QString::fromUtf8(e.what()))
                                                 .toStdString());
                }
                for (const auto &instance : project.instances)
                    definition_icons[instance.definition] = id;
            }
        }
        if (entry.value("hidden").toBool())
            continue;
        if (group.isEmpty())
            continue;
        QTreeWidgetItem *category = nullptr;
        std::string path;
        for (const auto &part : QString::fromUtf8(group).split('/')) {
            path += "/" + part.toStdString();
            auto *&group_item = categories[path];
            if (!group_item) {
                group_item = category ? new QTreeWidgetItem(category, {text(part.toUtf8().constData())})
                                      : new QTreeWidgetItem(library_, {text(part.toUtf8().constData())});
                group_item->setFirstColumnSpanned(true);
                auto font = group_item->font(0); font.setBold(true); group_item->setFont(0, font);
                group_item->setFlags(Qt::ItemIsEnabled);
                group_item->setExpanded(false);
            }
            category = group_item;
        }
        auto *item = new QTreeWidgetItem(category, {text(label.constData())});
        item->setIcon(0, component_icon(id));
        item->setData(0, Qt::UserRole, id);
    }
    search->setObjectName("library_search");
    connect(
        search, &QLineEdit::textChanged, this,
        [this, filtering = false, expanded = std::set<QTreeWidgetItem *>{}](const QString &query) mutable {
            const bool active = !query.trimmed().isEmpty();
            if (active && !filtering) {
                expanded.clear();
                for (QTreeWidgetItemIterator it(library_); *it; ++it)
                    if ((*it)->isExpanded()) expanded.insert(*it);
            }
            std::function<bool(QTreeWidgetItem *, bool)> filter = [&](QTreeWidgetItem *item, bool ancestor) {
                const bool own = !active || ancestor || item->text(0).contains(query.trimmed(), Qt::CaseInsensitive);
                bool any = own;
                for (int j = 0; j < item->childCount(); ++j) any |= filter(item->child(j), own);
                item->setHidden(!any);
                if (item->childCount() && (active || filtering)) item->setExpanded(active || expanded.count(item));
                return any;
            };
            for (int i = 0; i < library_->topLevelItemCount(); ++i) filter(library_->topLevelItem(i), false);
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
        "QToolBar{background:palette(window);border-bottom:1px solid palette(mid);spacing:5px;padding:4px;}"
        "QToolButton{color:palette(text);background:palette(base);border:1px solid "
        "palette(mid);border-radius:4px;padding:5px "
        "12px;}"
        "QToolButton:hover{background:palette(light);} "
        "QToolButton:disabled{color:palette(placeholder-text);}");
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
