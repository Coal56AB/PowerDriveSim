#include "apps/desktop/editor.hpp"
#include "apps/desktop/code_editor.hpp"
#include "apps/desktop/number_input.hpp"
#include "apps/desktop/theme.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QFontDatabase>
#include <QHeaderView>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QKeySequence>
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
#include <utility>
#include <vector>
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
class EventTableWidget final : public QTableWidget {
  public:
    explicit EventTableWidget(QWidget *parent = nullptr) : QTableWidget(1, 2, parent) {}

  protected:
    void keyPressEvent(QKeyEvent *event) override {
        if (!event->matches(QKeySequence::Paste)) {
            QTableWidget::keyPressEvent(event);
            return;
        }
        QString source = QApplication::clipboard()->text();
        for (const auto bracket : QString("{}[]()"))
            source.replace(bracket, ' ');
        const auto tokens = source.split(QRegularExpression(R"([,;\s]+)"), Qt::SkipEmptyParts);
        if (tokens.empty() || tokens.size() % 2 != 0) {
            QTableWidget::keyPressEvent(event);
            return;
        }
        std::vector<std::pair<QString, QString>> pairs;
        try {
            for (int i = 0; i < tokens.size(); i += 2) {
                const auto state = tokens[i + 1].trimmed();
                if (state != "0" && state != "1")
                    throw std::runtime_error("state");
                (void)parse_si(tokens[i].toStdString(), "s");
                pairs.emplace_back(tokens[i], state);
            }
        } catch (...) {
            QTableWidget::keyPressEvent(event);
            return;
        }
        const int first = currentRow() >= 0 ? currentRow() : 0;
        setRowCount(std::max(rowCount(), first + int(pairs.size()) + 1));
        for (int i = 0; i < int(pairs.size()); ++i) {
            setItem(first + i, 0, new QTableWidgetItem(pairs[size_t(i)].first));
            setItem(first + i, 1, new QTableWidgetItem(pairs[size_t(i)].second));
        }
        setCurrentCell(first + int(pairs.size()) - 1, 1);
        event->accept();
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
                    // Finish the complete dependent-property rebuild inside
                    // this UI event. Painting can only happen after it returns.
                    apply_inspector();
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
                auto *table = kind == "events" ? static_cast<QTableWidget *>(new EventTableWidget)
                                                : new QTableWidget(1, 2);
                table->setItemDelegateForColumn(0, new EventTimeDelegate(table));
                if (kind == "samples")
                    table->setItemDelegateForColumn(1, new EventTimeDelegate(table));
                widget = table;
                table->setHorizontalHeaderLabels(
                    {text("time_s"), text(kind == "events" ? "state" : "value")});
                table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
                table->verticalHeader()->hide();
                table->setMaximumHeight(160);
                if (kind == "events")
                    table->setToolTip(text("events_paste_hint"));
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
                auto *edit = kind == "code" ? static_cast<QPlainTextEdit *>(new CCodeEdit(true))
                                            : new QPlainTextEdit;
                widget = edit;
                edit->setMaximumHeight(kind == "code" ? 150 : 100);
                if (kind == "code") {
                    edit->setPlaceholderText(
                        "curr_ramp = ramp(0, 10, 0.008333333, 0.001111111);\n"
                        "phasepwm(50, 0.02, curr_ramp);");
                    edit->setStyleSheet(
                        "QPlainTextEdit{background:#111827;color:#d8dee9;border:1px solid #334155;"
                        "border-radius:6px;padding:8px;selection-background-color:#2563eb;"
                        "selection-color:#f8fafc;}");
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
