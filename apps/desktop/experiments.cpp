#include "apps/desktop/editor.hpp"
#include "core/experiment/sweep.hpp"
#include "formats/project/project.hpp"
#include "results/experiment_csv.hpp"
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <memory>
#include <sstream>

namespace pds::desktop {
namespace {
QString q(const std::string &s) { return QString::fromStdString(s); }

QString target_text(const ParameterTarget &target) {
    QStringList parts;
    for (const auto &id : target.instances)
        parts << q(id);
    if (!target.object.empty())
        parts << q(target.object);
    return parts.isEmpty() ? "profile" : parts.join("/");
}

ParameterTarget parse_target(const QStringList &parts, int first) {
    if (parts.size() <= first + 1)
        throw std::runtime_error("Expected object, field and value");
    ParameterTarget target;
    const auto object = parts[first];
    if (object != "profile" && object != "-" && object != ".") {
        auto path = object.split('/', Qt::SkipEmptyParts);
        if (path.isEmpty())
            throw std::runtime_error("Empty target object");
        target.object = path.takeLast().toStdString();
        for (const auto &step : path)
            target.instances.push_back(step.toStdString());
    }
    target.field = parts[first + 1].toStdString();
    return target;
}

QString axes_text(const Experiment &experiment) {
    QStringList lines;
    for (const auto &axis : experiment.axes) {
        QStringList cells{target_text(axis.target), q(axis.target.field)};
        for (double value : axis.values)
            cells << QString::number(value, 'g', 12);
        lines << cells.join(' ');
    }
    return lines.join('\n');
}

QString scenarios_text(const Experiment &experiment) {
    QStringList lines;
    for (const auto &scenario : experiment.scenarios) {
        QStringList overrides;
        for (const auto &value : scenario.overrides)
            overrides << (QStringList{target_text(value.target), q(value.target.field),
                                      QString::number(value.value, 'g', 12)}
                              .join(' '));
        lines << (q(scenario.name) + " | " + overrides.join("; "));
    }
    return lines.join('\n');
}

std::vector<SweepAxis> parse_axes(const QString &text) {
    std::vector<SweepAxis> axes;
    for (const auto &raw : text.split('\n', Qt::SkipEmptyParts)) {
        const auto parts = raw.simplified().split(' ', Qt::SkipEmptyParts);
        if (parts.isEmpty())
            continue;
        auto target = parse_target(parts, 0);
        SweepAxis axis{target, {}};
        for (int i = 2; i < parts.size(); ++i)
            axis.values.push_back(parse_si(parts[i].toStdString(), ""));
        axes.push_back(std::move(axis));
    }
    return axes;
}

std::vector<Scenario> parse_scenarios(const QString &text) {
    std::vector<Scenario> scenarios;
    for (const auto &raw : text.split('\n', Qt::SkipEmptyParts)) {
        const auto halves = raw.split('|');
        Scenario scenario;
        scenario.name = halves.front().trimmed().toStdString();
        if (scenario.name.empty())
            throw std::runtime_error("Scenario name is empty");
        if (halves.size() > 1) {
            for (const auto &entry : halves[1].split(';', Qt::SkipEmptyParts)) {
                const auto parts = entry.simplified().split(' ', Qt::SkipEmptyParts);
                auto target = parse_target(parts, 0);
                scenario.overrides.push_back({target, parse_si(parts.value(2).toStdString(), "")});
            }
        }
        scenarios.push_back(std::move(scenario));
    }
    return scenarios;
}

struct ExperimentRun {
    ExperimentProgress progress;
    std::vector<ExperimentCase> cases;
    QString unexpected_error;
};
} // namespace

void EditorWindow::show_experiments() {
    if (running() || !commit_inline_edit())
        return;
    QDialog dialog(this);
    dialog.setObjectName("experiments_dialog");
    dialog.setWindowTitle(text("experiments_title"));
    dialog.resize(980, 680);
    auto experiments = root_project().experiments;
    auto *layout = new QVBoxLayout(&dialog);
    auto *splitter = new QSplitter;
    layout->addWidget(splitter, 1);

    auto *left = new QWidget;
    auto *left_layout = new QVBoxLayout(left);
    auto *list = new QListWidget;
    list->setObjectName("experiments_list");
    left_layout->addWidget(list, 1);
    auto *add = new QPushButton(text("experiment_add"));
    add->setObjectName("experiment_add");
    auto *remove = new QPushButton(text("experiment_remove"));
    remove->setObjectName("experiment_remove");
    left_layout->addWidget(add);
    left_layout->addWidget(remove);
    splitter->addWidget(left);

    auto *right = new QWidget;
    auto *right_layout = new QVBoxLayout(right);
    auto *form = new QFormLayout;
    auto *name = new QLineEdit;
    name->setObjectName("experiment_name");
    auto *begin = new QLineEdit;
    begin->setObjectName("experiment_begin");
    auto *end = new QLineEdit;
    end->setObjectName("experiment_end");
    auto *retain = new QCheckBox(text("experiment_retain"));
    retain->setObjectName("experiment_retain");
    form->addRow(text("experiment_name"), name);
    form->addRow(text("experiment_begin"), begin);
    form->addRow(text("experiment_end"), end);
    form->addRow({}, retain);
    right_layout->addLayout(form);
    auto *axes = new QPlainTextEdit;
    axes->setObjectName("experiment_axes");
    axes->setMaximumHeight(105);
    auto *scenarios = new QPlainTextEdit;
    scenarios->setObjectName("experiment_scenarios");
    scenarios->setMaximumHeight(105);
    right_layout->addWidget(new QLabel(text("experiment_axes")));
    right_layout->addWidget(axes);
    right_layout->addWidget(new QLabel(text("experiment_scenarios")));
    right_layout->addWidget(scenarios);
    auto *channels = new QListWidget;
    channels->setObjectName("experiment_channels");
    channels->setMinimumHeight(130);
    right_layout->addWidget(new QLabel(text("experiment_channels")));
    right_layout->addWidget(channels);
    auto *progress = new QProgressBar;
    progress->setObjectName("experiment_progress");
    right_layout->addWidget(progress);
    auto *cases = new QTableWidget(0, 6);
    cases->setObjectName("experiment_cases");
    cases->setHorizontalHeaderLabels({"#", "status", "scenario", "t", "elapsed", "error"});
    cases->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    right_layout->addWidget(cases, 1);
    splitter->addWidget(right);
    splitter->setStretchFactor(1, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close);
    auto *run = buttons->addButton(text("experiment_run"), QDialogButtonBox::ActionRole);
    run->setObjectName("experiment_run");
    auto *export_csv = buttons->addButton(text("experiment_export"), QDialogButtonBox::ActionRole);
    export_csv->setObjectName("experiment_export");
    layout->addWidget(buttons);
    QPointer<QFutureWatcher<ExperimentRun>> active_watcher;
    std::shared_ptr<std::atomic_bool> active_cancel;
    connect(&dialog, &QDialog::finished, &dialog, [&] {
        if (active_watcher && active_watcher->isRunning()) {
            if (active_cancel)
                *active_cancel = true;
            active_watcher->waitForFinished();
        }
    });

    int active = -1;
    auto refresh_list = [&] {
        list->clear();
        for (const auto &experiment : experiments)
            new QListWidgetItem(q(experiment.name.empty() ? experiment.id : experiment.name), list);
        if (list->count())
            list->setCurrentRow(std::clamp(list->currentRow(), 0, list->count() - 1));
    };
    auto load_channels = [&](const Experiment *experiment) {
        channels->clear();
        try {
            for (const auto &channel : available_channels(compile(root_project()))) {
                auto *item = new QListWidgetItem(q(channel.name + " [" + channel.unit + "]"), channels);
                item->setData(Qt::UserRole, q(channel.object));
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                const bool checked = experiment && std::find(experiment->channels.begin(), experiment->channels.end(),
                                                             channel.object) != experiment->channels.end();
                item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
            }
        } catch (const Diagnostic &) {
        }
    };
    auto store_current = [&]() -> bool {
        const int row = active;
        if (row < 0 || row >= int(experiments.size()))
            return true;
        try {
            auto &experiment = experiments[size_t(row)];
            experiment.name = name->text().toStdString();
            experiment.begin = parse_si(begin->text().toStdString(), "s");
            experiment.end = parse_si(end->text().toStdString(), "s");
            experiment.retain_curves = retain->isChecked();
            experiment.axes = parse_axes(axes->toPlainText());
            experiment.scenarios = parse_scenarios(scenarios->toPlainText());
            experiment.channels.clear();
            for (int i = 0; i < channels->count(); ++i)
                if (channels->item(i)->checkState() == Qt::Checked)
                    experiment.channels.push_back(channels->item(i)->data(Qt::UserRole).toString().toStdString());
            (void)experiment_size(experiment);
            return true;
        } catch (const std::exception &e) {
            show_error(e);
            return false;
        }
    };
    auto load_current = [&] {
        const int row = active;
        const bool valid = row >= 0 && row < int(experiments.size());
        right->setEnabled(valid);
        run->setEnabled(valid);
        export_csv->setEnabled(valid);
        if (!valid) {
            name->clear();
            begin->clear();
            end->clear();
            axes->clear();
            scenarios->clear();
            channels->clear();
            return;
        }
        const auto &experiment = experiments[size_t(row)];
        name->setText(q(experiment.name));
        begin->setText(QString::number(experiment.begin, 'g', 12));
        end->setText(QString::number(experiment.end, 'g', 12));
        retain->setChecked(experiment.retain_curves);
        axes->setPlainText(axes_text(experiment));
        scenarios->setPlainText(scenarios_text(experiment));
        load_channels(&experiment);
        progress->setRange(0, int(std::max<size_t>(1, experiment_size(experiment))));
        progress->setValue(0);
        cases->setRowCount(0);
    };
    auto save_all = [&]() -> bool {
        if (!store_current())
            return false;
        try {
            document_->set_experiments(experiments);
            banner_->setText(text("experiment_saved"));
            update_title();
            return true;
        } catch (const std::exception &e) {
            show_error(e);
            return false;
        }
    };
    connect(list, &QListWidget::currentRowChanged, &dialog, [&](int row) {
        if (!store_current())
            return;
        active = row;
        load_current();
    });
    connect(add, &QPushButton::clicked, &dialog, [&] {
        if (!store_current())
            return;
        Experiment experiment;
        experiment.id = new_uuid();
        experiment.name = text("experiments_title").toStdString() + " " + std::to_string(experiments.size() + 1);
        experiment.begin = 0;
        experiment.end = -1;
        experiment.channels = project().scope_channels;
        experiments.push_back(std::move(experiment));
        refresh_list();
        list->setCurrentRow(list->count() - 1);
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        const int row = list->currentRow();
        if (row < 0 || row >= int(experiments.size()))
            return;
        experiments.erase(experiments.begin() + row);
        refresh_list();
        load_current();
    });
    auto run_experiment_from_dialog = [&](const QString &path) {
        if (!save_all())
            return;
        const int row = list->currentRow();
        if (row < 0 || row >= int(root_project().experiments.size()))
            return;
        auto experiment = root_project().experiments[size_t(row)];
        if (experiment.channels.empty()) {
            show_error(std::runtime_error(text("experiment_no_channels").toStdString()));
            return;
        }
        cases->setRowCount(0);
        progress->setValue(0);
        auto cancel = std::make_shared<std::atomic_bool>(false);
        auto *watcher = new QFutureWatcher<ExperimentRun>(&dialog);
        active_watcher = watcher;
        active_cancel = cancel;
        run->setEnabled(false);
        export_csv->setEnabled(false);
        buttons->button(QDialogButtonBox::Save)->setEnabled(false);
        auto *stop = buttons->addButton(text("stop"), QDialogButtonBox::ActionRole);
        connect(stop, &QPushButton::clicked, &dialog, [cancel] { *cancel = true; });
        connect(watcher, &QFutureWatcher<ExperimentRun>::finished, &dialog, [&, watcher, stop, path] {
            ExperimentRun result;
            try {
                result = watcher->result();
            } catch (const std::exception &error) {
                result.unexpected_error = QString::fromUtf8(error.what());
            } catch (...) {
                result.unexpected_error = text("unexpected_internal_error");
            }
            if (!result.unexpected_error.isEmpty())
                show_warning(result.unexpected_error);
            if (result.unexpected_error.isEmpty() && !path.isEmpty()) {
                std::ofstream out(path.toStdString());
                write_experiment_csv_header(out);
                for (const auto &entry : result.cases)
                    write_experiment_csv_case(out, entry);
            }
            for (const auto &entry : result.cases) {
                const int row_index = cases->rowCount();
                cases->insertRow(row_index);
                const QString status = entry.cancelled ? text("cancelled")
                    : entry.error_code.empty() ? text("complete") : q(entry.error_code);
                const QString error = entry.error_message.empty() ? QString() : q(entry.error_message);
                for (int column = 0; column < 6; ++column)
                    cases->setItem(row_index, column, new QTableWidgetItem);
                cases->item(row_index, 0)->setText(QString::number(entry.index));
                cases->item(row_index, 1)->setText(status);
                cases->item(row_index, 2)->setText(q(entry.scenario));
                cases->item(row_index, 3)->setText(QString::number(entry.simulated_time, 'g', 8));
                cases->item(row_index, 4)->setText(QString::number(entry.elapsed_seconds, 'f', 3));
                cases->item(row_index, 5)->setText(error);
            }
            progress->setValue(int(result.progress.completed + result.progress.failed));
            if (!result.unexpected_error.isEmpty())
                banner_->setText(text("warning_hint"));
            else
                banner_->setText(text(result.progress.cancelled ? "experiment_cancelled" : "experiment_complete")
                                     .arg(result.progress.completed)
                                     .arg(result.progress.total)
                                     .arg(result.progress.failed));
            buttons->removeButton(stop);
            delete stop;
            buttons->button(QDialogButtonBox::Save)->setEnabled(true);
            run->setEnabled(true);
            export_csv->setEnabled(true);
            active_watcher = nullptr;
            active_cancel.reset();
            watcher->deleteLater();
        });
        const auto unexpected_error = text("unexpected_internal_error");
        watcher->setFuture(QtConcurrent::run([source = root_project(), experiment, cancel, unexpected_error] {
            ExperimentRun result;
            try {
                result.progress = run_experiment(source, experiment, cancel.get(),
                                                 [&](ExperimentCase &&c) {
                                                     result.cases.push_back(std::move(c));
                                                 });
            } catch (const std::exception &error) {
                result.unexpected_error = QString::fromUtf8(error.what());
            } catch (...) {
                result.unexpected_error = unexpected_error;
            }
            return result;
        }));
        banner_->setText(text("experiment_running").arg(0).arg(experiment_size(experiment)));
    };
    connect(run, &QPushButton::clicked, &dialog, [&] { run_experiment_from_dialog({}); });
    connect(export_csv, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getSaveFileName(&dialog, text("experiment_export"), {}, "CSV (*.csv)");
        if (!path.isEmpty())
            run_experiment_from_dialog(path);
    });
    connect(buttons->button(QDialogButtonBox::Save), &QPushButton::clicked, &dialog, [&] { save_all(); });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    refresh_list();
    active = list->currentRow();
    load_current();
    if (experiments.empty())
        banner_->setText(text("experiment_empty"));
    dialog.exec();
}
} // namespace pds::desktop
