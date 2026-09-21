#pragma once

#include <QPlainTextEdit>
#include <QStringList>

class QCompleter;

namespace pds::desktop {

class CCodeEdit final : public QPlainTextEdit {
  public:
    explicit CCodeEdit(bool gate_functions, QWidget *parent = nullptr);

    void format_code();
    QCompleter *code_completer() const { return completer_; }

  protected:
    void keyPressEvent(QKeyEvent *event) override;

  private:
    QCompleter *completer_ = nullptr;
    QStringList base_completions_;
    QString completion_prefix() const;
    void insert_completion(const QString &completion);
    void update_completions();
};

} // namespace pds::desktop
