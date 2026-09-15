#pragma once
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>

namespace pds::desktop {
// Normalize before insertion, preserving QLineEdit's cursor, selection and undo history.
// Only attach to scalar fields; units and range checks remain with the field's parser.
class DecimalPointNormalizer final : public QObject {
  public:
    explicit DecimalPointNormalizer(QLineEdit *edit) : QObject(edit) { edit->installEventFilter(this); }
    bool eventFilter(QObject *object, QEvent *event) override {
        auto *edit = static_cast<QLineEdit *>(object);
        if (edit->isReadOnly())
            return false;
        auto paste = [edit] { edit->insert(QApplication::clipboard()->text().replace(',', '.')); };
        if (event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->matches(QKeySequence::Paste)) {
                paste();
                return true;
            }
            if (key->text().contains(',')) {
                QKeyEvent normalized(key->type(), key->key() == Qt::Key_Comma ? Qt::Key_Period : key->key(),
                                     key->modifiers(), QString(key->text()).replace(',', '.'),
                                     key->isAutoRepeat(), key->count());
                QApplication::sendEvent(edit, &normalized);
                return true;
            }
        } else if (event->type() == QEvent::InputMethod) {
            auto *input = static_cast<QInputMethodEvent *>(event);
            if (input->commitString().contains(',')) {
                QInputMethodEvent normalized(input->preeditString(), input->attributes());
                normalized.setCommitString(QString(input->commitString()).replace(',', '.'),
                                           input->replacementStart(), input->replacementLength());
                QApplication::sendEvent(edit, &normalized);
                return true;
            }
        } else if (event->type() == QEvent::ContextMenu) {
            QMenu menu(edit);
            menu.addAction(QLineEdit::tr("&Undo"), edit, &QLineEdit::undo)
                ->setEnabled(edit->isUndoAvailable());
            menu.addAction(QLineEdit::tr("&Redo"), edit, &QLineEdit::redo)
                ->setEnabled(edit->isRedoAvailable());
            menu.addSeparator();
            menu.addAction(QLineEdit::tr("Cu&t"), edit, &QLineEdit::cut)->setEnabled(edit->hasSelectedText());
            menu.addAction(QLineEdit::tr("&Copy"), edit, &QLineEdit::copy)
                ->setEnabled(edit->hasSelectedText());
            auto *action = menu.addAction(QLineEdit::tr("&Paste"), edit, paste);
            action->setObjectName("paste_number");
            action->setEnabled(!QApplication::clipboard()->text().isEmpty());
            menu.addAction(QLineEdit::tr("Delete"), edit, &QLineEdit::del)
                ->setEnabled(edit->hasSelectedText());
            menu.addSeparator();
            menu.addAction(QLineEdit::tr("Select All"), edit, &QLineEdit::selectAll)
                ->setEnabled(!edit->text().isEmpty());
            menu.exec(static_cast<QContextMenuEvent *>(event)->globalPos());
            return true;
        }
        return false;
    }
};
inline void normalize_decimal_point(QLineEdit *edit) {
    new DecimalPointNormalizer(edit);
}
} // namespace pds::desktop
