#include "apps/desktop/editor.hpp"
#include "core/model/hierarchy.hpp"
#include "apps/desktop/number_input.hpp"
#include "apps/desktop/theme.hpp"
#include "core/editor/properties.hpp"
#include "core/model/expression.hpp"
#include "core/model/c_program.hpp"
#include "formats/project/project.hpp"
#include "formats/samples/table.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPixmap>
#include <QScopedValueRollback>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
namespace pds::desktop {
namespace {
bool same_field_contract(const QJsonObject &a, const QJsonObject &b) {
    const QStringList keys{"key", "editor", "unit", "scale", "min", "max", "exclusiveMin", "options", "span", "group"};
    for (const auto &key : keys)
        if (a.value(key) != b.value(key))
            return false;
    return true;
}
void ensure_three_phase_source_variant(Project &project, unsigned connection) {
    constexpr const char *ids[]{"1a963f2c-ceb8-5cce-b927-44d735ec9e80",
                                "eb613164-faf4-5b03-9014-806885fef344"};
    const auto desired = std::string(ids[connection != 0]);
    if (std::any_of(project.definitions.begin(), project.definitions.end(),
                    [&](const Definition &definition) { return definition.id == desired; }))
        return;
    const QString file_name = connection ? ":/library/sources/three-phase-source-delta.pds"
                                         : ":/library/sources/three-phase-source-y.pds";
    QFile file(file_name);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error(file.errorString().toStdString());
    std::istringstream input(file.readAll().toStdString());
    const auto library = read_project(input);
    for (const auto &definition : library.definitions)
        if (std::none_of(project.definitions.begin(), project.definitions.end(),
                         [&](const Definition &existing) { return existing.id == definition.id; }))
            project.definitions.push_back(definition);
    if (std::none_of(project.definitions.begin(), project.definitions.end(),
                     [&](const Definition &definition) { return definition.id == desired; }))
        throw std::runtime_error("Three-phase source variant is missing from the library");
}
} // namespace
QWidget *EditorWindow::create_inspector_page() {
    auto *page = new QWidget;
    properties_ = new QFormLayout(page);
    properties_->setContentsMargins(16, 18, 16, 18);
    properties_->setVerticalSpacing(14);
    properties_->setRowWrapPolicy(QFormLayout::DontWrapRows);
    properties_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    inspector_hint_ = new QLabel(text("inspector_empty"));
    inspector_hint_->setWordWrap(true);
    inspector_hint_->setStyleSheet("color:palette(placeholder-text);padding:18px 0;");
    properties_->addRow(inspector_hint_);
    inspector_type_ = new QLabel;
    inspector_type_->setObjectName("property_native_type");
    inspector_type_->setStyleSheet("color:palette(placeholder-text);");
    properties_->addRow(text("native_type"), inspector_type_);

    property_editors_.clear();
    property_imports_.clear();
    active_fields_ = {};
    build_property_editors();

    apply_button_ = new QPushButton(text("apply"));
    apply_button_->setObjectName("apply_properties");
    properties_->addRow(apply_button_);
    compile_code_button_ = new QPushButton(text("compile_code"));
    compile_code_button_->setObjectName("compile_gate_code");
    compile_code_button_->hide();
    properties_->addRow(compile_code_button_);
    property_error_ = new QLabel;
    property_error_->setObjectName("property_error");
    property_error_->setStyleSheet("color:palette(bright-text)");
    property_error_->setWordWrap(true);
    properties_->addRow(property_error_);
    connect(apply_button_, &QPushButton::clicked, this, [this] { apply_inspector(); });
    connect(compile_code_button_, &QPushButton::clicked, this, [this] { compile_inspector_code(); });
    return page;
}
bool property_visible(const Project &project, const std::string &id, const QJsonObject &field) {
    const auto conditions = field.value("when").toObject();
    for (auto condition = conditions.begin(); condition != conditions.end(); ++condition) {
        PropertyValue value;
        try {
            value = read_property(project, id, condition.key().toStdString());
        } catch (const std::exception &) {
            return false;
        }
        unsigned current = 0;
        if (auto number = std::get_if<unsigned>(&value))
            current = *number;
        else if (auto flag = std::get_if<bool>(&value))
            current = *flag ? 1u : 0u;
        else
            return false;
        if (!condition.value().toArray().contains(int(current)))
            return false;
    }
    return true;
}
static QString field_text(QWidget *widget) {
    if (auto *button = qobject_cast<QPushButton *>(widget);
        button && widget->property("editor").toString() == "color")
        return button->property("color_value").toString();
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
static bool widget_changed(QWidget *widget) {
    if (widget->property("draft").toBool())
        return true;
    if (auto *line = qobject_cast<QLineEdit *>(widget))
        if (line->isModified())
            return true;
    return field_text(widget) != widget->property("loaded_text").toString();
}
static void set_field_text(QWidget *widget, const QString &text) {
    if (auto *button = qobject_cast<QPushButton *>(widget);
        button && widget->property("editor").toString() == "color") {
        const QColor color = QColor(text).isValid() ? QColor(text) : theme_colors().electrical;
        QPixmap swatch(24, 24);
        swatch.fill(color);
        button->setIcon(QIcon(swatch));
        button->setProperty("color_value", text);
        return;
    }
    if (auto *combo = qobject_cast<QComboBox *>(widget)) {
        if (text.isEmpty())
            combo->setCurrentIndex(-1);
        else
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
static void validate_numeric_range(double number, const QJsonObject &field, bool internal_value) {
    const double scale = internal_value ? field.value("scale").toDouble(1) : 1.;
    const double minimum = field.value("min").toDouble() / scale;
    const double maximum = field.value("max").toDouble() / scale;
    if (!std::isfinite(number) ||
        (field.contains("min") &&
         (number < minimum || (field.value("exclusiveMin").toBool() && number == minimum))) ||
        (field.contains("max") && number > maximum))
        throw std::runtime_error("Value outside configured range");
}
static PropertyValue parse_field(const QString &input, const QJsonObject &field) {
    const auto editor = field.value("editor").toString();
    if (editor == "text" || editor == "code")
        return input.toStdString();
    if (editor == "color") {
        const auto value = input.trimmed();
        if (value.isEmpty())
            return std::string{};
        const QColor color(value);
        if (!color.isValid())
            throw std::runtime_error("Expected a color such as #146cca");
        return color.name(QColor::HexRgb).toStdString();
    }
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
        validate_numeric_range(number, field, false);
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
    auto *old_page = inspector_stack_->currentWidget();
    auto *new_page = create_inspector_page();
    auto publish = [&] {
        properties_->activate();
        inspector_stack_->addWidget(new_page);
        inspector_stack_->setCurrentWidget(new_page);
        if (old_page) {
            inspector_stack_->removeWidget(old_page);
            // The retired page remains alive until the current widget signal
            // unwinds. Remove its lookup names immediately so all subsequent
            // queries resolve exclusively to the fully populated new page.
            for (auto *child : old_page->findChildren<QObject *>())
                child->setObjectName({});
            old_page->deleteLater();
        }
    };
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
        widget->setProperty("parameter_bound", false);
    }
    auto targets = selected_ids();
    std::erase_if(targets, [&](const std::string &id) { return !atoms_.count(id) && !wires_.count(id); });
    if (targets.empty() && (atoms_.count(selected_) || wires_.count(selected_)))
        targets.push_back(selected_);
    bool valid = !targets.empty();
    inspector_hint_->setVisible(!valid);
    inspector_hint_->setText(text("inspector_empty"));
    inspector_type_->setVisible(valid && targets.size() == 1);
    apply_button_->setVisible(valid);
    compile_code_button_->setVisible(false);
    if (!valid) {
        publish();
        return;
    }
    std::map<QString, QJsonObject> common;
    std::vector<QString> field_order;
    bool first = true;
    for (const auto &target : targets) {
        const auto type = object_type(project(), target);
        const auto &spec = component_specs_.at(type);
        std::map<QString, QJsonObject> visible;
        for (auto value : spec.value("fields").toArray()) {
            auto field = value.toObject();
            if (field.value("condition") == "internal_gate" && external_gate(project(), target))
                continue;
            if (!property_visible(project(), target, field))
                continue;
            visible[field.value("key").toString()] = field;
        }
        if (first) {
            common = visible;
            for (auto value : spec.value("fields").toArray()) {
                const auto key = value.toObject().value("key").toString();
                if (visible.count(key))
                    field_order.push_back(key);
            }
            first = false;
        } else {
            for (auto it = common.begin(); it != common.end();) {
                const auto found = visible.find(it->first);
                if (found == visible.end() || !same_field_contract(it->second, found->second))
                    it = common.erase(it);
                else
                    ++it;
            }
        }
    }
    const bool external = targets.size() == 1 && external_gate(project(), targets.front());
    if (external) {
        QString source;
        for (const auto &wire : project().wires) {
            Endpoint endpoint;
            if (wire.from == Endpoint{targets.front(), "gate"})
                endpoint = wire.to;
            else if (wire.to == Endpoint{targets.front(), "gate"})
                endpoint = wire.from;
            else
                continue;
            try {
                source = QString::fromStdString(
                    std::get<std::string>(read_property(project(), endpoint.object, "name")));
            } catch (const std::exception &) {
                source = QString();
            }
        }
        inspector_hint_->setText(text("gate_connected_source").arg(source));
        inspector_hint_->show();
    }
    if (targets.size() == 1) {
        const auto type = object_type(project(), targets.front());
        QString label = QString::fromStdString(type);
        if (type.rfind("instance:", 0) == 0) {
            const auto definition_id = type.substr(9);
            for (const auto &definition : project().definitions)
                if (definition.id == definition_id) {
                    label = QString::fromStdString(definition.name);
                    break;
                }
        } else if (auto spec = component_specs_.find(type); spec != component_specs_.end()) {
            const auto palette = spec->second.value("palette").toObject();
            if (!palette.isEmpty())
                label = text(palette.value("label").toString().toUtf8().constData());
        }
        inspector_type_->setText(label);
    }
    int row = 2;
    QString current_group;
    for (const auto &ordered_key : field_order) {
        const auto found = common.find(ordered_key);
        if (found == common.end())
            continue;
        auto field = found->second;
        auto key = field.value("key").toString();
        const auto group = field.value("group").toString().trimmed();
        if (!group.isEmpty() && group != current_group) {
            auto *heading = new QLabel(group);
            auto font = heading->font();
            font.setBold(true);
            heading->setFont(font);
            heading->setContentsMargins(0, current_group.isEmpty() ? 2 : 10, 0, 2);
            properties_->insertRow(row++, heading);
            current_group = group;
        }
        auto *widget = property_editors_.at(key);
        if (key == "name" && targets.size() == 1 && object_type(project(), targets.front()) == "tag")
            if (auto *line = qobject_cast<QLineEdit *>(widget)) {
                const auto current = std::find_if(project().tags.begin(), project().tags.end(),
                                                  [&](const ConnectionTag &tag) { return tag.id == targets.front(); });
                QStringList names;
                if (current != project().tags.end()) {
                    const auto flattened = flatten(root_project());
                    for (const auto &tag : flattened.project.tags) {
                        if (tag.listed && tag.domain == current->domain) {
                            const auto name = QString::fromStdString(tag.connection_name.empty()
                                                                        ? tag.name
                                                                        : tag.connection_name);
                            if (!names.contains(name))
                                names.push_back(name);
                        }
                    }
                }
                names.sort(Qt::CaseInsensitive);
                auto *completer = new QCompleter(names, line);
                completer->setCaseSensitivity(Qt::CaseInsensitive);
                completer->setFilterMode(Qt::MatchContains);
                line->setCompleter(completer);
            }
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
        QString contents;
        bool mixed = false;
        bool expression_bound = false;
        try {
            contents = display_value(read_property(project(), targets.front(), key.toStdString()), field);
            for (size_t i = 1; i < targets.size(); ++i)
                mixed |= display_value(read_property(project(), targets[i], key.toStdString()), field) != contents;
            if(targets.size()==1&&field.value("editor")=="number") {
                const auto binding=std::find_if(project().parameter_expressions.begin(),project().parameter_expressions.end(),
                    [&](const ParameterExpression &candidate){return candidate.object==targets.front()&&candidate.field==key.toStdString();});
                if(binding!=project().parameter_expressions.end()){contents=QString::fromStdString(binding->source);expression_bound=true;}
            }
        } catch (const std::exception &e) {
            property_error_->setText(QString::fromUtf8(e.what()));
            continue;
        }
        if (mixed)
            contents.clear();
        auto draft = targets.size() == 1 ? drafts_.find(targets.front()) : drafts_.end();
        const bool restored_draft = draft != drafts_.end() && draft->second.contains(key);
        if (restored_draft)
            contents = draft->second.value(key);
        set_field_text(widget, contents);
        widget->setProperty("loaded_text", contents);
        widget->setProperty("draft", restored_draft);
        widget->show();
        auto label = field.contains("displayLabel")
                         ? field.value("displayLabel").toString()
                         : text(field.value("label").toString().toUtf8().constData());
        if(expression_bound)label += "  ƒ";
        if (!hierarchy_path().empty()) {
            const auto &definition = pds::definition(root_project(), document_->current_definition());
            auto binding = std::find_if(definition.parameters.begin(), definition.parameters.end(),
                [&](const PublicParameter &parameter) {
                    return parameter.object == targets.front() && parameter.field == key.toStdString();
                });
            if (targets.size() == 1 && binding != definition.parameters.end()) {
                widget->setProperty("parameter_bound", true);
                widget->setToolTip(text("parameter_from_parent").arg(QString::fromStdString(binding->name)));
                label += "  ← " + QString::fromStdString(binding->name);
            }
        }
        if (field.value("span").toBool()) {
            properties_->insertRow(row++, widget);
            widget->setAccessibleName(label);
            widget->setToolTip(label);
        } else {
            auto *caption = new QLabel(label);
            caption->setToolTip(label);
            properties_->insertRow(row++, caption, widget);
        }
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
    compile_code_button_->setVisible(targets.size()==1&&common.contains("gate_code"));
    update_command_state();
    publish();
}
void EditorWindow::compile_inspector_code() {
    if(!editing_allowed()||selected_.empty()||!property_editors_.contains("gate_code"))return;
    try {
        CProgramOptions options;options.diagnostic_code="invalid_gate_script";options.object=selected_;
        options.allow_time=true;options.allow_gate_functions=true;options.require_return=true;
        const auto source=field_text(property_editors_.at("gate_code")).toStdString();
        (void)execute_c_program(compile_c_program(source,options),0);
        property_error_->setText(text("code_valid"));
    } catch(const std::exception &error) { property_error_->setText(QString::fromUtf8(error.what())); }
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
        auto targets = selected_ids();
        std::erase_if(targets, [&](const std::string &id) { return !atoms_.count(id) && !wires_.count(id); });
        if (targets.empty() && (atoms_.count(selected_) || wires_.count(selected_)))
            targets.push_back(selected_);
        std::vector<QJsonObject> changed_fields;
        for (auto entry : active_fields_) {
            auto field = entry.toObject();
            const auto key = field.value("key").toString();
            if (widget_changed(property_editors_.at(key)))
                changed_fields.push_back(field);
        }
        if (changed_fields.empty())
            return;
        document_->apply("Edit properties", [&](Project &p) {
            for (const auto &field : changed_fields) {
                const auto key = field.value("key").toString();
                const auto input=field_text(property_editors_.at(key));
                if(field.value("editor")=="number") {
                    try {
                        const auto value=parse_field(input,field);
                        for(const auto &target:targets) {
                            std::erase_if(p.parameter_expressions,[&](const ParameterExpression &binding){return binding.object==target&&binding.field==key.toStdString();});
                            write_property(p,target,key.toStdString(),value);
                        }
                    } catch(const std::exception &) {
                        CProgramOptions initialization_options;initialization_options.diagnostic_code="invalid_initialization";initialization_options.object=p.id;
                        const auto initialized=execute_c_program(compile_c_program(p.initialization_code,initialization_options));
                        std::map<std::string,std::string> variables;
                        for(const auto &[name,number]:initialized.variables){std::ostringstream encoded;encoded.precision(std::numeric_limits<double>::max_digits10);encoded<<number;variables.emplace(name,encoded.str());}
                        for(const auto &target:targets) {
                            const ExpressionOptions options{"invalid_parameter_expression",target,false,false};
                            const double value=evaluate_expression(input.toStdString(),variables,0,options);
                            validate_numeric_range(value, field, true);
                            write_property(p,target,key.toStdString(),value);
                            auto binding=std::find_if(p.parameter_expressions.begin(),p.parameter_expressions.end(),
                                [&](const ParameterExpression &candidate){return candidate.object==target&&candidate.field==key.toStdString();});
                            if(binding==p.parameter_expressions.end())p.parameter_expressions.push_back({target,key.toStdString(),input.toStdString()});
                            else binding->source=input.toStdString();
                        }
                    }
                } else {
                    const auto value = parse_field(input, field);
                    if (key == "three_phase_connection")
                        ensure_three_phase_source_variant(p, std::get<unsigned>(value));
                    for (const auto &target : targets)
                        write_property(p, target, key.toStdString(), value);
                }
            }
        });
        for (const auto &target : targets)
            drafts_.erase(target);
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
    if(field.value("editor")=="number") {
        const auto binding=std::find_if(project().parameter_expressions.begin(),project().parameter_expressions.end(),
            [&](const ParameterExpression &candidate){return candidate.object==id&&candidate.field==key;});
        if(binding!=project().parameter_expressions.end())edit->setText(QString::fromStdString(binding->source));
    }
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
            document_->apply("Edit label", [&](Project &p) {
                if(field.value("editor")=="number") {
                    try {
                        const auto value=parse_field(edit->text(),field);
                        std::erase_if(p.parameter_expressions,[&](const ParameterExpression &binding){return binding.object==id&&binding.field==key;});
                        write_property(p,id,key,value);
                    } catch(const std::exception &) {
                        const ExpressionOptions initialization_options{"invalid_initialization",p.id,false,false};
                        const auto program=parse_expression_program(p.initialization_code,initialization_options);
                        const ExpressionOptions options{"invalid_parameter_expression",id,false,false};
                        const double value=evaluate_expression(edit->text().toStdString(),program.variables,0,options);
                        validate_numeric_range(value, field, true);
                        write_property(p,id,key,value);
                        auto binding=std::find_if(p.parameter_expressions.begin(),p.parameter_expressions.end(),
                            [&](const ParameterExpression &candidate){return candidate.object==id&&candidate.field==key;});
                        if(binding==p.parameter_expressions.end())p.parameter_expressions.push_back({id,key,edit->text().toStdString()});
                        else binding->source=edit->text().toStdString();
                    }
                } else write_property(p,id,key,parse_field(edit->text(),field));
            });
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
