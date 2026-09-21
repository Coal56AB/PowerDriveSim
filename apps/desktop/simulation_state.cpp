#include "apps/desktop/editor.hpp"
#include "apps/desktop/code_editor.hpp"
#include "apps/desktop/number_input.hpp"
#include "core/model/expression.hpp"
#include "core/model/c_program.hpp"
#include "formats/snapshot/snapshot.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QVBoxLayout>
#include <array>
#include <cmath>
#include <sstream>

namespace pds::desktop {
void EditorWindow::show_expression_settings() {
    if(running()||!commit_inline_edit())return;
    QDialog dialog(this);dialog.setObjectName("expression_settings_dialog");
    dialog.setWindowTitle(text("expression_settings"));dialog.resize(720,500);
    auto *layout=new QVBoxLayout(&dialog);
    auto *hint=new QLabel(text("expression_hint"));hint->setWordWrap(true);layout->addWidget(hint);
    auto *code=new CCodeEdit(false);code->setObjectName("expression_initialization_code");
    code->setPlainText(QString::fromStdString(project().initialization_code));
    code->setPlaceholderText("const double udc = 540;\ndouble load = 10;\n");
    layout->addWidget(code,1);
    auto *error=new QLabel;error->setObjectName("expression_error");error->setWordWrap(true);layout->addWidget(error);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);
    auto *format=buttons->addButton(text("format_code"),QDialogButtonBox::ActionRole);
    format->setObjectName("format_initialization_code");
    auto *compile=buttons->addButton(text("compile_code"),QDialogButtonBox::ActionRole);
    compile->setObjectName("compile_initialization_code");
    buttons->button(QDialogButtonBox::Cancel)->setText(text("dialog_cancel"));layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(format,&QPushButton::clicked,code,&CCodeEdit::format_code);
    connect(code,&QPlainTextEdit::textChanged,error,&QLabel::clear);
    connect(compile,&QPushButton::clicked,&dialog,[&]{
        try {
            CProgramOptions options;options.diagnostic_code="invalid_initialization";options.object=project().id;
            (void)execute_c_program(compile_c_program(code->toPlainText().toStdString(),options));
            error->setText(text("code_valid"));
        } catch(const std::exception &exception) { error->setText(QString::fromUtf8(exception.what())); }
    });
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{
        try {
            const auto source=code->toPlainText().toStdString();
            CProgramOptions options;options.diagnostic_code="invalid_initialization";options.object=project().id;
            (void)execute_c_program(compile_c_program(source,options));
            document_->apply("Initialization variables",[&](Project &p){p.initialization_code=source;});
            dialog.accept();
        } catch(const std::exception &exception) { error->setText(QString::fromUtf8(exception.what())); }
    });
    if(dialog.exec()==QDialog::Accepted)refresh();
}

void EditorWindow::show_step_settings() {
    if (running() || !commit_inline_edit())
        return;
    QDialog dialog(this);
    dialog.setObjectName("step_settings_dialog");
    dialog.setWindowTitle(text("step_settings"));
    dialog.setMinimumWidth(470);
    auto *layout = new QVBoxLayout(&dialog);
    auto *adaptive = new QCheckBox(text("adaptive_enabled"));
    adaptive->setObjectName("adaptive_enabled");
    adaptive->setChecked(project().profile.step_control.adaptive);
    layout->addWidget(adaptive);
    auto *form = new QFormLayout;
    const auto &control = project().profile.step_control;
    struct Field {
        const char *key;
        const char *unit;
        double value;
        QLineEdit *edit = nullptr;
    };
    Field fields[] = {
        {"step_maximum", "s", project().profile.step},     {"step_minimum", "s", control.minimum_step},
        {"step_relative", "", control.relative_tolerance}, {"step_voltage", "V", control.voltage_tolerance},
        {"step_current", "A", control.current_tolerance},  {"step_charge", "C", control.charge_tolerance}};
    for (auto &field : fields) {
        field.edit = new QLineEdit(QString::number(field.value, 'g', 12));
        field.edit->setObjectName(field.key);
        normalize_decimal_point(field.edit);
        if (&field == &fields[0])
            field.edit->setText(step_->text());
        else {
            field.edit->setEnabled(adaptive->isChecked());
            connect(adaptive, &QCheckBox::toggled, field.edit, &QWidget::setEnabled);
        }
        form->addRow(text(field.key), field.edit);
    }
    layout->addLayout(form);
    auto *hint = new QLabel(text("adaptive_hint"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *error = new QLabel;
    error->setObjectName("step_error");
    error->setWordWrap(true);
    layout->addWidget(error);
    for (auto &field : fields)
        connect(field.edit, &QLineEdit::textChanged, error, &QLabel::clear);
    connect(adaptive, &QCheckBox::toggled, error, &QLabel::clear);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Cancel)->setText(text("dialog_cancel"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            auto profile = project().profile;
            std::array<double, 6> values;
            for (size_t k = 0; k < values.size(); ++k)
                values[k] = k == 0 || adaptive->isChecked()
                                ? parse_si(fields[k].edit->text().toStdString(), fields[k].unit)
                                : fields[k].value;
            profile.step = values[0];
            profile.step_control = {
                adaptive->isChecked(), values[1], values[2], values[3], values[4], values[5]};
            if (!std::isfinite(profile.step) || profile.step <= 0)
                throw std::runtime_error(text("positive_time").toStdString());
            validate_step_control(profile, root_project().id);
            document_->apply("Integration step", [&](Project &p) { p.profile = profile; });
            step_->setModified(false);
            dialog.accept();
        } catch (const std::exception &e) {
            error->setText(QString::fromUtf8(e.what()));
        }
    });
    if (dialog.exec() == QDialog::Accepted)
        refresh();
}

void EditorWindow::show_initial_settings() {
    if (running() || !commit_inline_edit())
        return;
    QDialog dialog(this);
    dialog.setObjectName("initial_settings_dialog");
    dialog.setWindowTitle(text("initial_settings"));
    dialog.setMinimumWidth(460);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *mode = new QComboBox;
    mode->setObjectName("initial_state_mode");
    mode->addItems({text("initial_specified"), text("initial_zero"), text("initial_dc")});
    mode->setCurrentIndex(static_cast<int>(project().profile.initial_state));
    form->addRow(text("initial_mode"), mode);
    auto *warmup = new QLineEdit(QString::number(project().profile.warmup, 'g', 12));
    warmup->setObjectName("initial_warmup");
    normalize_decimal_point(warmup);
    form->addRow(text("initial_warmup"), warmup);
    layout->addLayout(form);
    auto *hint = new QLabel(text("initial_hint"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *error = new QLabel;
    error->setObjectName("initial_error");
    error->setWordWrap(true);
    layout->addWidget(error);
    connect(warmup, &QLineEdit::textChanged, error, &QLabel::clear);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Cancel)->setText(text("dialog_cancel"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            const double value = parse_si(warmup->text().toStdString(), "s");
            const double stop = parse_si(stop_->text().toStdString(), "s");
            if (!std::isfinite(value) || value < 0 || value >= stop)
                throw std::runtime_error(text("initial_warmup_error").toStdString());
            document_->apply("Initial state", [&](Project &p) {
                p.profile.initial_state = static_cast<InitialState>(mode->currentIndex());
                p.profile.warmup = value;
            });
            dialog.accept();
        } catch (const std::exception &e) {
            error->setText(QString::fromUtf8(e.what()));
        }
    });
    if (dialog.exec() == QDialog::Accepted)
        refresh();
}

void EditorWindow::continue_simulation() {
    if (!running() && continuation_)
        launch_simulation(continuation_);
}

void EditorWindow::step_simulation() {
    if (running()) {
        if (paused_.load()) {
            step_after_pause_ = true;
            discard_continuation_on_finish_ = false;
            cancel_ = true;
            paused_ = false;
            run_->setEnabled(false);
        }
        return;
    }
    try {
        auto state = continuation_;
        if (state && state->time >= parse_si(stop_->text().toStdString(), "s")) state.reset();
        launch_simulation(state, 1);
    } catch (const std::exception &error) { show_error(error); }
}

bool EditorWindow::save_simulation_snapshot(const QString &path) {
    if (running() || !continuation_)
        return false;
    try {
        std::ostringstream output;
        write_snapshot(*continuation_, output);
        const auto bytes = output.str();
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(bytes.data(), static_cast<qint64>(bytes.size())) !=
                static_cast<qint64>(bytes.size()) ||
            !file.commit())
            throw std::runtime_error(file.errorString().toStdString());
        banner_->setText(text("snapshot_saved").arg(continuation_->time, 0, 'g', 12));
        return true;
    } catch (const std::exception &e) {
        show_error(e);
        return false;
    }
}

bool EditorWindow::load_simulation_snapshot(const QString &path) {
    if (running() || !commit_inline_edit())
        return false;
    try {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error(file.errorString().toStdString());
        std::istringstream input(file.readAll().toStdString());
        auto state = read_snapshot(input);
        auto model = root_project();
        model.profile.stop = parse_si(stop_->text().toStdString(), "s");
        model.profile.step = parse_si(step_->text().toStdString(), "s");
        model.profile.method = method_->currentIndex() ? Method::trapezoidal : Method::backward_euler;
        validate_snapshot(state, compile(model));
        clear_result();
        continuation_ = std::move(state);
        banner_->setText(text("snapshot_loaded").arg(continuation_->time, 0, 'g', 12));
        update_command_state();
        return true;
    } catch (const std::exception &e) {
        show_error(e);
        return false;
    }
}
} // namespace pds::desktop
