#pragma once

#include <QPlainTextEdit>
#include <QStringList>
#include <functional>

class QCompleter;
class QMouseEvent;
class QPaintEvent;
class QWidget;

namespace pds::desktop {

void show_c_code_reference(QWidget *parent = nullptr);

class CCodeEdit final : public QPlainTextEdit {
  public:
    explicit CCodeEdit(bool time_context, QWidget *parent = nullptr);

    void format_code();
    void enable_expand(std::function<void()> callback, const QString &tooltip);
    void set_gate_outputs(unsigned count) { gate_outputs_ = count; }
    QCompleter *code_completer() const { return completer_; }

  protected:
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

  private:
    QCompleter *completer_ = nullptr;
    std::function<void()> expand_callback_;
    QString expand_tooltip_;
    QStringList base_completions_;
    unsigned gate_outputs_ = 1;
    QString completion_prefix() const;
    void insert_completion(const QString &completion);
    void update_completions();
    void show_search(bool replace);
    QRect expand_rect() const;
};

} // namespace pds::desktop
