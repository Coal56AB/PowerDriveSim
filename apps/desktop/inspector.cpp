#include "apps/desktop/editor.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGraphicsScene>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <sstream>
namespace pds::desktop {
void EditorWindow::commit_profile() {
    if (rebuilding_ || running() || !document_)
        return;
    try {
        auto profile = project().profile;
        profile.stop = parse_si(stop_->text().toStdString(), "s");
        profile.step = parse_si(step_->text().toStdString(), "s");
        profile.method = method_->currentIndex() ? Method::trapezoidal : Method::backward_euler;
        if (profile.stop <= 0 || profile.step <= 0)
            throw std::runtime_error(text("positive_time").toStdString());
        if (profile == project().profile)
            return;
        document_->apply("Simulation profile", [&](Project &p) { p.profile = profile; });
        stop_->setModified(false);
        step_->setModified(false);
        stop_->setToolTip(text("time_s"));
        step_->setToolTip(text("time_s"));
        refresh();
    } catch (const std::exception &e) {
        stop_->setToolTip(QString::fromUtf8(e.what()));
        step_->setToolTip(QString::fromUtf8(e.what()));
        banner_->setText(QString::fromUtf8(e.what()));
    }
}
void EditorWindow::update_command_state() {
    if (!document_)
        return;
    for (const auto &[id, action] : component_actions_)
        action->setEnabled(editing_allowed());
    for (const auto &[key, widget] : property_editors_)
        if (auto *line = qobject_cast<QLineEdit *>(widget))
            line->setReadOnly(!editing_allowed());
        else
            widget->setEnabled(editing_allowed());
    for (auto *field : {stop_, step_})
        if (field)
            field->setReadOnly(running());
    if (method_)
        method_->setEnabled(!running());
    if (apply_button_)
        apply_button_->setEnabled(editing_allowed());

    auto *focus = QApplication::focusWidget();
    bool editing = qobject_cast<QLineEdit *>(focus) || qobject_cast<QPlainTextEdit *>(focus) ||
                   qobject_cast<QSpinBox *>(focus);
    for (const char *id : {"delete", "rotate", "rotate_back", "mirror", "copy", "cut", "paste", "duplicate",
                           "select_all", "undo", "redo", "fit", "fit_selection", "actual_size"}) {
        auto it = commands_.find(id);
        if (it == commands_.end())
            continue;
        bool enabled = !editing;
        if (std::string(id) == "undo")
            enabled &= document_ && document_->can_undo() && !running();
        else if (std::string(id) == "redo")
            enabled &= document_ && document_->can_redo() && !running();
        else if (std::string(id) != "fit" && std::string(id) != "fit_selection" &&
                 std::string(id) != "actual_size" && std::string(id) != "copy" &&
                 std::string(id) != "select_all")
            enabled &= editing_allowed();
        it->second->setEnabled(enabled);
    }
    refresh_hierarchy();
}
bool EditorWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == this && event->type() == QEvent::WindowDeactivate && canvas_ &&
        canvas_->editing_gesture() && !paste_fragment_)
        canvas_->cancel_gesture();

    if (event->type() == QEvent::ShortcutOverride) {
        auto *key = static_cast<QKeyEvent *>(event);
        auto *field = qobject_cast<QLineEdit *>(watched);
        if (field && !running() && key->key() == Qt::Key_Escape) {
            key->accept();
            return true;
        }
    }
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        auto *field = qobject_cast<QLineEdit *>(watched);
        if (field && !running() && key->key() == Qt::Key_Escape) {
            if (field == inline_editor_) {
                cancel_inline_edit();
                canvas_->setFocus();
            } else if (field == stop_ || field == step_) {
                field->setText(QString::number(
                    field == stop_ ? project().profile.stop : project().profile.step, 'g', 12));
            } else if (std::any_of(property_editors_.begin(), property_editors_.end(),
                                   [&](const auto &item) { return item.second == field; })) {
                drafts_.erase(selected_);
                field->setModified(false);
                fill_inspector();
            } else
                return QMainWindow::eventFilter(watched, event);
            key->accept();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}
} // namespace pds::desktop
