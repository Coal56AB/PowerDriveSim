#include "apps/desktop/code_editor.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QCompleter>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStringListModel>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextCharFormat>
#include <array>

namespace pds::desktop {
namespace {

class CCodeHighlighter final : public QSyntaxHighlighter {
  public:
    explicit CCodeHighlighter(QTextDocument *document) : QSyntaxHighlighter(document) {
        keyword_.setForeground(QColor("#7aa2f7"));
        keyword_.setFontWeight(QFont::DemiBold);
        type_.setForeground(QColor("#2ac3de"));
        function_.setForeground(QColor("#73daca"));
        number_.setForeground(QColor("#ff9e64"));
        string_.setForeground(QColor("#9ece6a"));
        comment_.setForeground(QColor("#6b7280"));
        declaration_.setForeground(QColor("#c0caf5"));
        punctuation_.setForeground(QColor("#bb9af7"));
    }

  protected:
    void highlightBlock(const QString &source) override {
        apply(source, R"(\b(if|else|for|while|break|continue|return|true|false)\b)", keyword_);
        apply(source, R"(\b(auto|bool|const|double|float|int|void)\b)", type_);
        apply(source, R"(\b(abs|fabs|sqrt|sin|cos|tan|asin|acos|atan|atan2|exp|log|log10|floor|ceil|round|pow|fmod|min|fmin|max|fmax|clamp|pwm|phasepwm|square|ramp)\s*(?=\())", function_);
        apply(source, R"((?<![A-Za-z_])(?:\d+(?:\.\d*)?|\.\d+)(?:e[-+]?\d+)?)", number_);
        apply(source, R"(\b[A-Za-z_][A-Za-z0-9_]*(?=\s*(?:=|\()))", declaration_);
        apply(source, R"([{}=;,()\[\]])", punctuation_);
        apply(source, R"("(?:\\.|[^"\\])*")", string_);

        int start = 0;
        if (previousBlockState() != 1)
            start = source.indexOf("/*");
        while (start >= 0) {
            const int end = source.indexOf("*/", start + 2);
            const int length = end < 0 ? source.size() - start : end - start + 2;
            setFormat(start, length, comment_);
            if (end < 0) {
                setCurrentBlockState(1);
                break;
            }
            start = source.indexOf("/*", start + length);
        }
        if (const auto line = source.indexOf("//"); line >= 0)
            setFormat(line, source.size() - line, comment_);
    }

  private:
    QTextCharFormat keyword_, type_, function_, number_, string_, comment_, declaration_, punctuation_;
    void apply(const QString &source, const QString &pattern, const QTextCharFormat &format) {
        QRegularExpression expression(pattern);
        auto matches = expression.globalMatch(source);
        while (matches.hasNext()) {
            const auto match = matches.next();
            setFormat(match.capturedStart(), match.capturedLength(), format);
        }
    }
};

QStringList common_completions() {
    return {
        "auto", "bool", "break", "clamp(value, minimum, maximum)", "const double", "continue",
        "double", "else", "false", "for (int i = 0; i < count; ++i) {\n    \n}", "if (condition) {\n    \n}",
        "int", "max(a, b)", "min(a, b)", "pow(value, exponent)", "return", "sqrt(value)",
        "true", "while (condition) {\n    \n}", "abs(value)", "sin(value)", "cos(value)"
    };
}

} // namespace

CCodeEdit::CCodeEdit(bool gate_functions, QWidget *parent) : QPlainTextEdit(parent) {
    base_completions_ = common_completions();
    if (gate_functions)
        base_completions_ << "phasepwm(frequency, duty, phase)" << "pwm(frequency, duty, delay)"
                          << "ramp(t0, t1, value0, value1)" << "square(frequency, duty, delay)"
                          << "stime" << "t";
    base_completions_.sort(Qt::CaseInsensitive);
    completer_ = new QCompleter(base_completions_, this);
    completer_->setWidget(this);
    completer_->setCompletionMode(QCompleter::PopupCompletion);
    completer_->setCaseSensitivity(Qt::CaseInsensitive);
    completer_->setFilterMode(Qt::MatchStartsWith);
    connect(completer_, QOverload<const QString &>::of(&QCompleter::activated), this,
            [this](const QString &completion) { insert_completion(completion); });

    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setTabStopDistance(QFontMetricsF(font()).horizontalAdvance(' ') * 4);
    new CCodeHighlighter(document());

    auto *format = new QAction(tr("Format code"), this);
    format->setObjectName("format_c_code");
    format->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    format->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(format);
    connect(format, &QAction::triggered, this, &CCodeEdit::format_code);
}

QString CCodeEdit::completion_prefix() const {
    auto cursor = textCursor();
    cursor.select(QTextCursor::WordUnderCursor);
    return cursor.selectedText();
}

void CCodeEdit::update_completions() {
    QStringList completions = base_completions_;
    if(gate_outputs_>1)completions << "IN[ind]";
    const auto cursor = textCursor();
    const auto prefix = completion_prefix();
    const auto source = toPlainText().left(cursor.position() - prefix.size());
    const std::array<QRegularExpression, 2> declarations{
        QRegularExpression(R"(\b(?:const\s+)?(?:auto|bool|double|float|int)\s+([A-Za-z_][A-Za-z0-9_]*))"),
        QRegularExpression(R"(\b(?:bool|double|float|int|void)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()")};
    for (const auto &declaration : declarations) {
        auto matches = declaration.globalMatch(source);
        while (matches.hasNext()) {
            const auto word = matches.next().captured(1);
            if (word != prefix && !completions.contains(word, Qt::CaseInsensitive))
                completions.push_back(word);
        }
    }
    completions.sort(Qt::CaseInsensitive);
    static_cast<QStringListModel *>(completer_->model())->setStringList(completions);
}

void CCodeEdit::insert_completion(const QString &completion) {
    auto cursor = textCursor();
    cursor.select(QTextCursor::WordUnderCursor);
    cursor.insertText(completion);
    setTextCursor(cursor);
}

void CCodeEdit::keyPressEvent(QKeyEvent *event) {
    if (completer_->popup()->isVisible()) {
        switch (event->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Escape:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            event->ignore();
            return;
        default:
            break;
        }
    }
    const bool explicit_request = event->key() == Qt::Key_Space &&
                                  (event->modifiers() & Qt::ControlModifier);
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        const auto line = textCursor().block().text();
        const auto leading = line.left(line.size() - line.trimmed().size());
        QPlainTextEdit::keyPressEvent(event);
        auto cursor = textCursor();
        cursor.insertText(leading + (line.trimmed().endsWith('{') ? "    " : ""));
        setTextCursor(cursor);
        return;
    }
    if (event->key() == Qt::Key_Tab && !(event->modifiers() & Qt::ControlModifier)) {
        insertPlainText("    ");
        return;
    }
    const bool popup_was_visible = completer_->popup()->isVisible();
    if (!explicit_request)
        QPlainTextEdit::keyPressEvent(event);
    const auto prefix = completion_prefix();
    if (!explicit_request && !popup_was_visible && prefix.isEmpty()) {
        completer_->popup()->hide();
        return;
    }
    update_completions();
    completer_->setCompletionPrefix(prefix);
    if (completer_->completionCount() == 0) {
        completer_->popup()->hide();
        return;
    }
    auto rect = cursorRect();
    rect.translate(0, fontMetrics().height() + 6);
    rect.setWidth(completer_->popup()->sizeHintForColumn(0) +
                  completer_->popup()->verticalScrollBar()->sizeHint().width() + 18);
    completer_->complete(rect);
}

void CCodeEdit::format_code() {
    const auto original_cursor = textCursor();
    const auto lines = toPlainText().split('\n');
    QStringList formatted;
    int indentation = 0;
    for (const auto &source : lines) {
        const auto line = source.trimmed();
        if (line.startsWith('}'))
            indentation = std::max(0, indentation - 1);
        formatted.push_back(QString(indentation * 4, ' ') + line);
        int delta = 0;
        bool quoted = false;
        for (int i = 0; i < line.size(); ++i) {
            if (line[i] == '"' && (i == 0 || line[i - 1] != '\\'))
                quoted = !quoted;
            if (quoted || (i + 1 < line.size() && line.mid(i, 2) == "//"))
                continue;
            if (line[i] == '{') ++delta;
            if (line[i] == '}') --delta;
        }
        indentation = std::max(0, indentation + delta + (line.startsWith('}') ? 1 : 0));
    }
    setPlainText(formatted.join('\n'));
    auto cursor = textCursor();
    cursor.setPosition(std::min(original_cursor.position(), document()->characterCount() - 1));
    setTextCursor(cursor);
}

} // namespace pds::desktop
