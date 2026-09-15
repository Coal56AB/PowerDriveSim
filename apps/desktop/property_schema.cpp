#include "apps/desktop/editor.hpp"
#include "apps/desktop/number_input.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QTableWidget>
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
            if (key.isEmpty() || !keys.insert(key).second ||
                !QStringList{"text", "number", "bool", "integer", "events", "points"}.contains(editor) ||
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
            } else if (kind == "bool") {
                auto *check = new QCheckBox;
                widget = check;
                connect(check, &QCheckBox::clicked, this, [this] { apply_inspector(); });
            } else if (kind == "integer") {
                auto *spin = new QSpinBox;
                widget = spin;
                spin->setMinimumHeight(34);
                connect(spin, &QSpinBox::editingFinished, this, [this] {
                    if (!inspector_loading_)
                        apply_inspector();
                });
            } else if (kind == "events") {
                auto *table = new QTableWidget(1, 2);
                table->setItemDelegateForColumn(0, new EventTimeDelegate(table));
                widget = table;
                table->setHorizontalHeaderLabels({text("time_s"), text("state")});
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
            } else {
                auto *edit = new QPlainTextEdit;
                widget = edit;
                edit->setMaximumHeight(100);
                edit->setPlaceholderText("x y");
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
