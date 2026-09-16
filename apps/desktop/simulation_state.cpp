#include "apps/desktop/editor.hpp"
#include "formats/snapshot/snapshot.hpp"
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QSaveFile>
#include <sstream>

namespace pds::desktop {
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
