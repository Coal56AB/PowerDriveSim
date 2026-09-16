#include "apps/desktop/editor.hpp"
#include "apps/desktop/number_input.hpp"
#include "apps/desktop/theme.hpp"
#include "core/editor/properties.hpp"
#include "formats/samples/table.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSpinBox>
#include <QTableWidget>
#include <algorithm>
#include <cmath>
#include <sstream>
namespace pds::desktop {
bool property_visible(const Project &project, const std::string &id, const QJsonObject &field) {
    const auto conditions = field.value("when").toObject();
    for (auto condition = conditions.begin(); condition != conditions.end(); ++condition) {
        const auto current = std::get<unsigned>(read_property(project, id, condition.key().toStdString()));
        if (!condition.value().toArray().contains(int(current)))
            return false;
    }
    return true;
}
static QString field_text(QWidget *widget) {
    if (auto *combo = qobject_cast<QComboBox *>(widget))
        return combo->currentData().toString();
    if (auto *line = qobject_cast<QLineEdit *>(widget))
        return line->text();
    if (auto *check = qobject_cast<QCheckBox *>(widget))
        return check->isChecked() ? "1" : "0";
    if (auto *spin = qobject_cast<QSpinBox *>(widget))
        return QString::number(spin->value());
    if (auto *edit = qobject_cast<QPlainTextEdit *>(widget))
        return edit->toPlainText();
    auto *table = qobject_cast<QTableWidget *>(widget);
    QString result;
    for (int row = 0; row < table->rowCount(); ++row) {
        QStringList parts;
        for (int col = 0; col < 2; ++col)
            parts << (table->item(row, col) ? table->item(row, col)->text() : QString());
        if (!parts.join("").trimmed().isEmpty())
            result += parts.join(widget->property("editor") == "samples" ? "\t" : " ") + "\n";
    }
    return result;
}
static void set_field_text(QWidget *widget, const QString &text) {
    if (auto *combo = qobject_cast<QComboBox *>(widget)) {
        combo->setCurrentIndex(combo->findData(text.toUInt()));
        return;
    }
    if (auto *line = qobject_cast<QLineEdit *>(widget)) {
        line->setText(text);
        line->setModified(false);
        return;
    }
    if (auto *check = qobject_cast<QCheckBox *>(widget)) {
        check->setChecked(text == "1");
        return;
    }
    if (auto *spin = qobject_cast<QSpinBox *>(widget)) {
        spin->setValue(text.toInt());
        return;
    }
    if (auto *edit = qobject_cast<QPlainTextEdit *>(widget)) {
        edit->setPlainText(text);
        return;
    }
    auto *table = qobject_cast<QTableWidget *>(widget);
    auto lines = text.split('\n', Qt::SkipEmptyParts);
    table->setRowCount(lines.size() + 1);
    for (int row = 0; row < table->rowCount(); ++row) {
        auto parts = row < lines.size()
                         ? (widget->property("editor") == "samples" ? lines[row].split('\t')
                                                                    : lines[row].simplified().split(' '))
                         : QStringList{};
        for (int col = 0; col < 2; ++col)
            table->setItem(row, col, new QTableWidgetItem(col < parts.size() ? parts[col] : QString()));
    }
}
static QString display_value(const PropertyValue &value, const QJsonObject &field) {
    if (auto v = std::get_if<std::string>(&value))
        return QString::fromStdString(*v);
    if (auto v = std::get_if<bool>(&value))
        return *v ? "1" : "0";
    if (auto v = std::get_if<unsigned>(&value))
        return QString::number(*v);
    if (auto v = std::get_if<double>(&value)) {
        auto unit = field.value("unit").toString();
        double number = *v * field.value("scale").toDouble(1);
        return unit.isEmpty() ? QString::number(number, 'g', 12)
                              : engineering_value(number, unit.toStdString());
    }
    QString result;
    if (auto v = std::get_if<std::vector<GateEvent>>(&value))
        for (auto e : *v)
            result += QString::number(e.time, 'g', 12) + "s " + (e.closed ? "1" : "0") + "\n";
    if (auto v = std::get_if<std::vector<Point>>(&value))
        for (auto e : *v)
            result += QString::number(e.x, 'g', 17) + (field.value("editor") == "samples" ? "\t" : " ") +
                      QString::number(e.y, 'g', 17) + "\n";
    return result;
}
static PropertyValue parse_field(const QString &input, const QJsonObject &field) {
    const auto editor = field.value("editor").toString();
    if (editor == "text")
        return input.toStdString();
    if (editor == "bool")
        return input == "1";
    if (editor == "enum") {
        bool valid = false;
        auto number = input.toUInt(&valid);
        bool found = false;
        for (const auto &option : field.value("options").toArray())
            found |= option.toObject().value("value").toInt() == int(number);
        if (!valid || !found)
            throw std::runtime_error("Invalid choice");
        return number;
    }
    if (editor == "number" || editor == "integer") {
        double number = parse_si(input.toStdString(), field.value("unit").toString().toStdString());
        if ((field.contains("min") &&
             (number < field.value("min").toDouble() ||
              (field.value("exclusiveMin").toBool() && number == field.value("min").toDouble()))) ||
            (field.contains("max") && number > field.value("max").toDouble()))
            throw std::runtime_error("Value outside configured range");
        if (editor == "integer") {
            if (number < 0 || number != std::floor(number) || number > 4294967295.)
                throw std::runtime_error("Expected a non-negative integer");
            return static_cast<unsigned>(number);
        }
        return number / field.value("scale").toDouble(1);
    }
    std::vector<GateEvent> events;
    std::vector<Point> points;
    for (auto line : input.split('\n')) {
        if (line.trimmed().isEmpty())
            continue;
        std::istringstream in(line.toStdString());
        std::string extra;
        if (editor == "events") {
            std::string time;
            int state = -1;
            if (!(in >> time >> state) || (in >> extra) || (state != 0 && state != 1))
                throw std::runtime_error(text("events_format").toStdString());
            events.push_back({parse_si(time, "s"), "", state != 0});
        } else if (editor == "samples") {
            const auto cells = line.split('\t');
            if (cells.size() != 2)
                throw std::runtime_error(text("samples_format").toStdString());
            points.push_back(
                {parse_si(cells[0].toStdString(), "s"),
                 parse_si(cells[1].toStdString(), field.value("unit").toString().toStdString())});
        } else {
            Point point;
            if (!(in >> point.x >> point.y) || (in >> extra) || !std::isfinite(point.x) ||
                !std::isfinite(point.y))
                throw std::runtime_error(text("bends_format").toStdString());
            points.push_back(point);
        }
    }
    if (editor == "events")
        return events;
    return points;
}
void EditorWindow::fill_inspector() {
    if (inspector_loading_)
        return;
    if (inspector_id_ != selected_)
        remember_draft();
    QScopedValueRollback<bool> loading(inspector_loading_, true);
    inspector_id_ = selected_;
    property_error_->clear();
    active_fields_ = {};
    for (auto &[key, button] : property_imports_) {
        if (properties_->indexOf(button) >= 0) {
            auto row = properties_->takeRow(button);
            delete row.fieldItem;
        }
        button->hide();
    }
    for (auto &[key, widget] : property_editors_) {
        if (properties_->indexOf(widget) >= 0) {
            auto row = properties_->takeRow(widget);
            if (row.labelItem) {
                delete row.labelItem->widget();
                delete row.labelItem;
            }
            delete row.fieldItem;
        }
        widget->hide();
        widget->setProperty("draft", false);
    }
    bool valid = atoms_.count(selected_) || wires_.count(selected_);
    inspector_hint_->setVisible(!valid);
    inspector_hint_->setText(text("inspector_empty"));
    apply_button_->setVisible(valid);
    if (!valid)
        return;
    const auto type = object_type(project(), selected_);
    const auto &spec = component_specs_.at(type);
    const bool external = external_gate(project(), selected_);
    if (external) {
        QString source;
        for (const auto &wire : project().wires) {
            Endpoint endpoint;
            if (wire.from == Endpoint{selected_, "gate"})
                endpoint = wire.to;
            else if (wire.to == Endpoint{selected_, "gate"})
                endpoint = wire.from;
            else
                continue;
            source = QString::fromStdString(
                std::get<std::string>(read_property(project(), endpoint.object, "name")));
        }
        inspector_hint_->setText(text("gate_connected_source").arg(source));
        inspector_hint_->show();
    }
    int row = 1;
    for (auto value : spec.value("fields").toArray()) {
        auto field = value.toObject();
        if (field.value("condition") == "internal_gate" && external)
            continue;
        if (!property_visible(project(), selected_, field))
            continue;
        auto key = field.value("key").toString();
        auto *widget = property_editors_.at(key);
        if (auto *combo = qobject_cast<QComboBox *>(widget)) {
            combo->clear();
            for (const auto &entry : field.value("options").toArray()) {
                const auto option = entry.toObject();
                combo->addItem(text(option.value("label").toString().toUtf8().constData()),
                               unsigned(option.value("value").toInt()));
            }
        }
        if (auto *spin = qobject_cast<QSpinBox *>(widget))
            spin->setRange(field.value("min").toInt(0), field.value("max").toInt(2147483647));
        auto contents = display_value(read_property(project(), selected_, key.toStdString()), field);
        auto draft = drafts_.find(selected_);
        if (draft != drafts_.end() && draft->second.contains(key))
            contents = draft->second.value(key);
        set_field_text(widget, contents);
        widget->show();
        const auto label = field.contains("displayLabel")
                               ? field.value("displayLabel").toString()
                               : text(field.value("label").toString().toUtf8().constData());
        if (field.value("span").toBool()) {
            properties_->insertRow(row++, widget);
            widget->setAccessibleName(label);
            widget->setToolTip(label);
        } else
            properties_->insertRow(row++, label, widget);
        if (field.value("editor") == "samples") {
            auto *table = qobject_cast<QTableWidget *>(widget);
            table->setHorizontalHeaderLabels(
                {text("time_s"), text("value") + ", " + field.value("unit").toString()});
            auto *button = property_imports_.at(key);
            properties_->insertRow(row++, button);
            button->show();
        }
        active_fields_.append(field);
    }
}
void EditorWindow::import_samples(const QString &key) {
    if (!editing_allowed() || inspector_id_ != selected_ || selected_.empty())
        return;
    QJsonObject field;
    for (const auto &entry : active_fields_)
        if (entry.toObject().value("key").toString() == key)
            field = entry.toObject();
    if (field.value("editor") != "samples")
        return;
    const auto owner = selected_;
    const auto path =
        QFileDialog::getOpenFileName(this, text("import_samples"), {}, text("sample_file_filter"));
    if (path.isEmpty() || selected_ != owner)
        return;
    try {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error(file.errorString().toStdString());
        if (file.size() > qint64(sample_table_max_bytes))
            throw std::runtime_error("Table exceeds 16 MiB");
        auto bytes = file.readAll();
        if (file.error() != QFileDevice::NoError)
            throw std::runtime_error(file.errorString().toStdString());
        std::istringstream input(bytes.toStdString());
        const auto points = read_sample_table(input, field.value("unit").toString().toStdString());
        auto *widget = property_editors_.at(key);
        {
            QScopedValueRollback<bool> loading(inspector_loading_, true);
            set_field_text(widget, display_value(points, field));
        }
        widget->setProperty("draft", true);
        remember_draft();
        property_error_->clear();
    } catch (const std::exception &e) {
        property_error_->setText(text("sample_import_error").arg(QString::fromUtf8(e.what())));
    }
}
void EditorWindow::apply_inspector() {
    if (!editing_allowed() || selected_.empty() || applying_ || inspector_loading_)
        return;
    QScopedValueRollback<bool> guard(applying_, true);
    remember_draft();
    try {
        document_->apply("Edit properties", [&](Project &p) {
            for (auto entry : active_fields_) {
                auto field = entry.toObject();
                auto key = field.value("key").toString();
                write_property(p, selected_, key.toStdString(),
                               parse_field(field_text(property_editors_.at(key)), field));
            }
        });
        drafts_.erase(selected_);
        property_error_->clear();
        refresh();
    } catch (const std::exception &e) {
        property_error_->setText(QString::fromUtf8(e.what()));
        show_error(e);
    }
}
void EditorWindow::remember_draft() {
    if (inspector_loading_ || inspector_id_.empty())
        return;
    QMap<QString, QString> values;
    bool modified = drafts_.count(inspector_id_) != 0;
    for (auto entry : active_fields_) {
        auto key = entry.toObject().value("key").toString();
        auto *widget = property_editors_.at(key);
        values[key] = field_text(widget);
        modified |= widget->property("draft").toBool();
        if (auto *line = qobject_cast<QLineEdit *>(widget))
            modified |= line->isModified();
    }
    if (modified)
        drafts_[inspector_id_] = values;
}
void EditorWindow::cancel_inline_edit() {
    if (!inline_editor_)
        return;
    auto *edit = inline_editor_.data();
    inline_editor_ = nullptr;
    edit->disconnect(this);
    edit->hide();
    edit->deleteLater();
}
bool EditorWindow::commit_inline_edit() {
    if (inline_editor_)
        QMetaObject::invokeMethod(inline_editor_, "editingFinished", Qt::DirectConnection);
    return !inline_editor_;
}
void EditorWindow::edit_inline(const std::string &id, const QJsonObject &field, QRect rect) {
    if (running())
        return;
    cancel_inline_edit();
    canvas_->cancel_gesture();
    auto *edit = new QLineEdit(canvas_->viewport());
    if (field.value("editor") == "number" || field.value("editor") == "integer")
        normalize_decimal_point(edit);
    inline_editor_ = edit;
    edit->setObjectName("inline_property");
    const auto key = field.value("key").toString().toStdString();
    edit->setText(display_value(read_property(project(), id, key), field));
    rect.setWidth(std::clamp(rect.width() + 24, 120, 320));
    rect.setHeight(32);
    rect.moveLeft(std::clamp(rect.left(), 0, std::max(0, canvas_->viewport()->width() - rect.width())));
    edit->setGeometry(rect);
    edit->show();
    edit->setFocus();
    edit->selectAll();
    connect(edit, &QLineEdit::editingFinished, this, [this, edit, id, key, field] {
        if (inline_editor_ != edit)
            return;
        try {
            auto value = parse_field(edit->text(), field);
            document_->apply("Edit label", [&](Project &p) { write_property(p, id, key, value); });
            drafts_.erase(id);
            cancel_inline_edit();
            refresh();
        } catch (const std::exception &e) {
            edit->setStyleSheet("border:1px solid palette(bright-text);");
            edit->setToolTip(QString::fromUtf8(e.what()));
        }
    });
}
} // namespace pds::desktop
