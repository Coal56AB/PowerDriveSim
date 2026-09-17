#include "apps/desktop/editor.hpp"
#include "apps/desktop/number_input.hpp"
#include "apps/desktop/theme.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QFontDatabase>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPixmap>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QSyntaxHighlighter>
#include <QTableWidget>
#include <QTextCharFormat>
#include <array>
namespace pds::desktop {
namespace {
class EventTimeDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override {
        auto *editor = QStyledItemDelegate::createEditor(parent, option, index);
        if (auto *line = qobject_cast<QLineEdit *>(editor))
            normalize_decimal_point(line);
        return editor;
    }
};
class GateCodeHighlighter final : public QSyntaxHighlighter {
  public:
    explicit GateCodeHighlighter(QTextDocument *document) : QSyntaxHighlighter(document) {
        keyword_.setForeground(QColor("#7aa2f7"));
        keyword_.setFontWeight(QFont::DemiBold);
        function_.setForeground(QColor("#73daca"));
        number_.setForeground(QColor("#ff9e64"));
        comment_.setForeground(QColor("#6b7280"));
        variable_.setForeground(QColor("#c0caf5"));
        assign_.setForeground(QColor("#bb9af7"));
    }

  protected:
    void highlightBlock(const QString &text) override {
        apply(R"(\b(auto|bool|double|float|int|return|true|false)\b)", keyword_);
        apply(R"(\b(pwm|phasepwm|square|ramp)\s*(?=\())", function_);
        apply(R"((?<![A-Za-z_])[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:e[-+]?\d+)?)", number_);
        apply(R"(\b[A-Za-z_][A-Za-z0-9_]*(?=\s*=))", variable_);
        apply(R"([=;,()])", assign_);
        if (const auto start = text.indexOf("//"); start >= 0)
            setFormat(start, text.size() - start, comment_);
    }

  private:
    QTextCharFormat keyword_, function_, number_, comment_, variable_, assign_;
    void apply(const QString &pattern, const QTextCharFormat &format) {
        QRegularExpression re(pattern);
        auto it = re.globalMatch(currentBlock().text());
        while (it.hasNext()) {
            const auto match = it.next();
            setFormat(match.capturedStart(), match.capturedLength(), format);
        }
    }
};
class GateCodeEdit final : public QPlainTextEdit {
  public:
    explicit GateCodeEdit(QWidget *parent = nullptr) : QPlainTextEdit(parent) {
        completer_ = new QCompleter(QStringList{
                                        "phasepwm(frequency, duty, delay)",
                                        "pwm(frequency, duty, delay)",
                                        "square(frequency, duty, delay)",
                                        "ramp(t0, t1, value0, value1)",
                                        "curr_ramp",
                                        "t",
                                        "true",
                                        "false",
                                    },
                                    this);
        completer_->setWidget(this);
        completer_->setCompletionMode(QCompleter::PopupCompletion);
        completer_->setCaseSensitivity(Qt::CaseInsensitive);
        completer_->setFilterMode(Qt::MatchStartsWith);
        connect(completer_, QOverload<const QString &>::of(&QCompleter::activated), this,
                [this](const QString &completion) { insert_completion(completion); });
    }

  protected:
    void keyPressEvent(QKeyEvent *event) override {
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
        const bool explicit_request =
            event->key() == Qt::Key_Space && (event->modifiers() & Qt::ControlModifier);
        if (!explicit_request)
            QPlainTextEdit::keyPressEvent(event);
        const auto prefix = completion_prefix();
        if (!explicit_request && prefix.size() < 1) {
            completer_->popup()->hide();
            return;
        }
        completer_->setCompletionPrefix(prefix);
        if (completer_->completionCount() == 0) {
            completer_->popup()->hide();
            return;
        }
        auto rect = cursorRect();
        rect.setWidth(completer_->popup()->sizeHintForColumn(0) +
                      completer_->popup()->verticalScrollBar()->sizeHint().width() + 18);
        completer_->complete(rect);
    }

  private:
    QCompleter *completer_ = nullptr;
    QString completion_prefix() const {
        auto cursor = textCursor();
        cursor.select(QTextCursor::WordUnderCursor);
        return cursor.selectedText();
    }
    void insert_completion(const QString &completion) {
        auto cursor = textCursor();
        cursor.select(QTextCursor::WordUnderCursor);
        cursor.insertText(completion);
        setTextCursor(cursor);
    }
};
} // namespace
void EditorWindow::load_component_specs() {
    const auto override = qEnvironmentVariable("PDS_COMPONENT_DIR");
    QDir external(override.isEmpty() ? QCoreApplication::applicationDirPath() + "/components" : override);
    QDir bundled(":/components");
    auto files = bundled.entryList({"*.json"}, QDir::Files);
    for (const auto &name : external.entryList({"*.json"}, QDir::Files))
        if (!files.contains(name))
            files.push_back(name);
    for (const auto &name : files) {
        QFile file(external.exists(name) ? external.filePath(name) : bundled.filePath(name));
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error(file.errorString().toStdString());
        QJsonParseError error;
        auto document = QJsonDocument::fromJson(file.readAll(), &error);
        auto spec = document.object();
        auto type = spec.value("type").toString().toStdString();
        if (error.error != QJsonParseError::NoError || type.empty() || !spec.value("fields").isArray() ||
            component_specs_.count(type))
            throw std::runtime_error(("Invalid component configuration: " + file.fileName()).toStdString());
        std::set<QString> keys;
        for (auto value : spec.value("fields").toArray()) {
            auto field = value.toObject();
            auto key = field.value("key").toString(), editor = field.value("editor").toString();
            if (editor == "enum" && field.value("options").toArray().isEmpty())
                throw std::runtime_error(("Empty choices in " + file.fileName()).toStdString());
            if (key.isEmpty() || !keys.insert(key).second ||
                !QStringList{"text", "number", "bool", "integer", "enum", "events", "points", "samples", "code", "color"}
                     .contains(editor) ||
                field.value("scale").toDouble(1) <= 0)
                throw std::runtime_error(("Invalid property in " + file.fileName()).toStdString());
        }
        component_specs_[type] = spec;
    }
}
void EditorWindow::build_property_editors() {
    for (const auto &[type, spec] : component_specs_)
        for (auto value : spec.value("fields").toArray()) {
            auto field = value.toObject();
            auto key = field.value("key").toString(), kind = field.value("editor").toString();
            if (property_editors_.count(key)) {
                if (property_editors_.at(key)->property("editor").toString() != kind)
                    throw std::runtime_error("Inconsistent property editor: " + key.toStdString());
                continue;
            }
            QWidget *widget = nullptr;
            if (kind == "text" || kind == "number") {
                auto *line = new QLineEdit;
                if (kind == "number")
                    normalize_decimal_point(line);
                widget = line;
                connect(line, &QLineEdit::editingFinished, this, [this] {
                    if (!inspector_loading_ && !applying_)
                        apply_inspector();
                });
                connect(line, &QLineEdit::textEdited, this, [this] { remember_draft(); });
            } else if (kind == "color") {
                auto *button = new QPushButton;
                button->setFixedSize(38, 32);
                button->setIconSize({24, 24});
                button->setToolTip(text("wire_color_hint"));
                button->setContextMenuPolicy(Qt::CustomContextMenu);
                widget = button;
                auto set_swatch = [button](const QString &value) {
                    const QColor color = QColor(value).isValid() ? QColor(value) : theme_colors().electrical;
                    QPixmap swatch(24, 24);
                    swatch.fill(color);
                    button->setIcon(QIcon(swatch));
                    button->setProperty("color_value", value);
                };
                set_swatch({});
                connect(button, &QPushButton::clicked, this, [this, button, set_swatch] {
                    QColor initial(button->property("color_value").toString());
                    if (!initial.isValid())
                        initial = theme_colors().electrical;
                    const std::array<QColor, 8> defaults{
                        QColor("#146cca"), QColor("#17866d"), QColor("#8c67c8"), QColor("#c56819"),
                        QColor("#b8394e"), QColor("#263c55"), QColor("#ffffff"), QColor("#000000")};
                    QColorDialog::setCustomColor(0, initial.rgb());
                    for (int i = 0; i < int(defaults.size()); ++i)
                        QColorDialog::setCustomColor(i + 1, defaults[size_t(i)].rgb());
                    QColorDialog dialog(initial, this);
                    dialog.setWindowTitle(text("wire_color"));
                    dialog.setOption(QColorDialog::DontUseNativeDialog);
                    if (dialog.exec() == QDialog::Accepted) {
                        set_swatch(dialog.selectedColor().name(QColor::HexRgb));
                        button->setProperty("draft", true);
                        apply_inspector();
                    }
                });
                connect(button, &QWidget::customContextMenuRequested, this, [this, button, set_swatch](QPoint point) {
                    QMenu menu(button);
                    auto *automatic = menu.addAction(text("wire_color_auto"));
                    connect(automatic, &QAction::triggered, this, [this, button, set_swatch] {
                        set_swatch({});
                        button->setProperty("draft", true);
                        apply_inspector();
                    });
                    menu.exec(button->mapToGlobal(point));
                });
            } else if (kind == "enum") {
                auto *combo = new QComboBox;
                widget = combo;
                connect(combo, &QComboBox::activated, this, [this, combo] {
                    combo->setProperty("draft", true);
                    QTimer::singleShot(0, this, [this] { apply_inspector(); });
                });
            } else if (kind == "bool") {
                auto *check = new QCheckBox;
                widget = check;
                connect(check, &QCheckBox::clicked, this, [this, check] {
                    check->setProperty("draft", true);
                    apply_inspector();
                });
            } else if (kind == "integer") {
                auto *spin = new QSpinBox;
                widget = spin;
                spin->setMinimumHeight(34);
                connect(spin, &QSpinBox::editingFinished, this, [this] {
                    if (!inspector_loading_)
                        apply_inspector();
                });
            } else if (kind == "events" || kind == "samples") {
                auto *table = new QTableWidget(1, 2);
                table->setItemDelegateForColumn(0, new EventTimeDelegate(table));
                if (kind == "samples")
                    table->setItemDelegateForColumn(1, new EventTimeDelegate(table));
                widget = table;
                table->setHorizontalHeaderLabels(
                    {text("time_s"), text(kind == "events" ? "state" : "value")});
                table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
                table->verticalHeader()->hide();
                table->setMaximumHeight(160);
                connect(table, &QTableWidget::cellChanged, this, [this, table](int row, int) {
                    if (inspector_loading_)
                        return;
                    if (row == table->rowCount() - 1)
                        table->setRowCount(row + 2);
                    table->setProperty("draft", true);
                    remember_draft();
                });
                if (kind == "samples") {
                    auto *button = new QPushButton(text("import_samples"));
                    button->setObjectName("import_" + key);
                    button->setToolTip(text("import_samples_hint"));
                    button->setAutoDefault(false);
                    properties_->addRow(button);
                    property_imports_[key] = button;
                    connect(button, &QPushButton::clicked, this, [this, key] { import_samples(key); });
                }
            } else {
                auto *edit = kind == "code" ? static_cast<QPlainTextEdit *>(new GateCodeEdit)
                                            : new QPlainTextEdit;
                widget = edit;
                edit->setMaximumHeight(kind == "code" ? 150 : 100);
                if (kind == "code") {
                    edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
                    edit->setTabStopDistance(QFontMetricsF(edit->font()).horizontalAdvance(' ') * 4);
                    edit->setPlaceholderText(
                        "curr_ramp = ramp(0, 10, 0.008333333, 0.001111111);\n"
                        "phasepwm(50, 0.02, curr_ramp);");
                    edit->setStyleSheet(
                        "QPlainTextEdit{background:#111827;color:#d8dee9;border:1px solid #334155;"
                        "border-radius:6px;padding:8px;selection-background-color:#2563eb;"
                        "selection-color:#f8fafc;}");
                    new GateCodeHighlighter(edit->document());
                } else {
                    edit->setPlaceholderText("x y");
                }
                connect(edit, &QPlainTextEdit::textChanged, this, [this, edit] {
                    if (!inspector_loading_) {
                        edit->setProperty("draft", true);
                        remember_draft();
                    }
                });
            }
            widget->setObjectName("property_" + key);
            widget->setProperty("editor", kind);
            properties_->addRow(text(field.value("label").toString().toUtf8().constData()), widget);
            property_editors_[key] = widget;
        }
    // Stable aliases used by canvas commands and existing UI integrations; fields are created from JSON.
    auto widget = [&](const char *key) -> QWidget * {
        auto i = property_editors_.find(key);
        return i == property_editors_.end() ? nullptr : i->second;
    };
    auto line = [&](const char *key) { return qobject_cast<QLineEdit *>(widget(key)); };
    name_ = line("name");
    value_ = line("value");
}
} // namespace pds::desktop
