#pragma once
#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QTimer>
#include <QToolButton>
#include <functional>

namespace pds::desktop {
// Delay name clicks until a possible double click has been resolved. The eye
// remains an immediate toggle, and keyboard activation uses the normal action.
class CurveNameButton : public QToolButton {
  public:
    explicit CurveNameButton(QWidget *parent) : QToolButton(parent), click_(this) {
        click_.setSingleShot(true);
        connect(&click_, &QTimer::timeout, this, [this] { defaultAction()->trigger(); });
    }
    std::function<void()> rename;

  protected:
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) {
            QToolButton::mouseReleaseEvent(event);
            return;
        }
        if (suppress_release_) {
            suppress_release_ = false;
            setDown(false);
            return;
        }
        if (event->position().x() < iconSize().width() + 10) {
            QToolButton::mouseReleaseEvent(event);
            return;
        }
        const bool activate = isDown() && rect().contains(event->position().toPoint());
        setDown(false);
        if (activate)
            click_.start(QApplication::doubleClickInterval());
        event->accept();
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && event->position().x() >= iconSize().width() + 10) {
            click_.stop();
            suppress_release_ = true;
            setDown(false);
            if (rename)
                rename();
            event->accept();
        } else
            QToolButton::mouseDoubleClickEvent(event);
    }
    void mousePressEvent(QMouseEvent *event) override {
        suppress_release_ = false;
        QToolButton::mousePressEvent(event);
    }

  private:
    QTimer click_;
    bool suppress_release_ = false;
};
class CurveNameEdit : public QLineEdit {
  public:
    using QLineEdit::QLineEdit;
    std::function<void()> cancel;

  protected:
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            // QLineEdit lets Return reach QDialog's default button after emitting
            // editingFinished. Inline rename must consume it completely.
            event->accept();
            emit editingFinished();
        } else if (event->key() == Qt::Key_Escape) {
            if (cancel)
                cancel();
            event->accept();
        } else
            QLineEdit::keyPressEvent(event);
    }
};
} // namespace pds::desktop
