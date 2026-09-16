#include "apps/desktop/editor.hpp"
#include "apps/desktop/number_input.hpp"
#include "formats/snapshot/snapshot.hpp"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QVBoxLayout>
#include <cmath>
#include <sstream>

namespace pds::desktop {
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
    if (!running())
        launch_simulation(continuation_, 1);
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
