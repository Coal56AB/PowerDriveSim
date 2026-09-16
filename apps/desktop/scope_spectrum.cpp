#include "apps/desktop/editor.hpp"
#include "apps/desktop/number_input.hpp"
#include "apps/desktop/ui_icons.hpp"
#include "results/spectrum.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFileDialog>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSaveFile>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <sstream>

namespace pds::desktop {
void Scope::show_spectrum() {
    if (spectrum_) {
        spectrum_->show();
        spectrum_->raise();
        return;
    }
    auto *dialog = new QDialog(this);
    spectrum_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName("scope_spectrum");
    dialog->setWindowTitle(text("spectrum"));
    dialog->resize(1000, 770);
    auto *box = new QVBoxLayout(dialog);
    auto *form = new QGridLayout;
    box->addLayout(form);
    auto *signal = new QComboBox;
    signal->setObjectName("spectrum_channel");
    if (result_)
        for (int ch : channels_) {
            const auto c = result_channel(*result_, ch);
            signal->addItem(curve_name(c.object) + QString::fromStdString(" [" + c.unit + "]"),
                            QString::fromStdString(c.object));
        }
    form->addWidget(new QLabel(text("measurement_signal")), 0, 0);
    form->addWidget(signal, 0, 1, 1, 3);
    auto make_edit = [&](const char *key, double value, int row, int column, const QString &label) {
        auto *edit = new QLineEdit(QString::number(value, 'g', 12));
        normalize_decimal_point(edit);
        edit->setObjectName(key);
        form->addWidget(new QLabel(label), row, column);
        form->addWidget(edit, row, column + 1);
        return edit;
    };
    auto *from = make_edit("spectrum_begin", begin, 1, 0, text("spectrum_begin"));
    auto *to = make_edit("spectrum_end", end, 1, 2, text("spectrum_end"));
    auto *frequency = make_edit("spectrum_fundamental", 0, 2, 0, text("spectrum_fundamental"));
    auto *count = new QSpinBox;
    count->setObjectName("spectrum_harmonics");
    count->setRange(2, 1000);
    count->setValue(40);
    form->addWidget(new QLabel(text("spectrum_harmonics")), 2, 2);
    form->addWidget(count, 2, 3);
    auto *size = new QComboBox;
    size->setObjectName("spectrum_size");
    for (int n = 256; n <= 65536; n *= 2)
        size->addItem(QString::number(n), n);
    size->setCurrentIndex(4); // 4096 points.
    form->addWidget(new QLabel(text("spectrum_size")), 1, 4);
    form->addWidget(size, 1, 5);
    auto *window = new QComboBox;
    window->setObjectName("spectrum_window");
    window->addItems({text("spectrum_rectangular"), "Hann", "Hamming", "Blackman"});
    window->setCurrentIndex(1);
    form->addWidget(new QLabel(text("spectrum_window")), 2, 4);
    form->addWidget(window, 2, 5);
    auto *range = new QComboBox;
    range->setObjectName("spectrum_range");
    range->addItems({text("visible_range"), text("cursor_range"), text("full_range")});
    form->addWidget(new QLabel(text("spectrum_use_range")), 0, 4);
    form->addWidget(range, 0, 5);
    connect(range, &QComboBox::activated, this, [this, from, to](int index) {
        double a = begin, b = end;
        if (index == 1) {
            a = std::min(cursor_a, cursor_b);
            b = std::max(cursor_a, cursor_b);
        } else if (index == 2 && result_ && !result_->samples.empty()) {
            a = result_->samples.front().time;
            b = result_->samples.back().time;
        }
        from->setText(QString::number(a, 'g', 12));
        to->setText(QString::number(b, 'g', 12));
    });
    auto *whole = new QCheckBox(text("spectrum_whole_periods"));
    whole->setObjectName("spectrum_whole_periods");
    whole->setChecked(true);
    form->addWidget(whole, 3, 0, 1, 3);
    auto *status = new QLabel;
    status->setObjectName("spectrum_status");
    status->setWordWrap(true);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    box->addWidget(status);
    auto *splitter = new QSplitter(Qt::Vertical);
    box->addWidget(splitter, 1);
    auto *plot_page = new QWidget;
    auto *plot_box = new QVBoxLayout(plot_page);
    plot_box->setContentsMargins(0, 0, 0, 0);
    auto *plot = new Scope(plot_page, Domain::frequency);
    plot->setObjectName("spectrum_plot");
    plot->set_wheel_modifiers(wheel_x_, wheel_y_);
    auto *navigation = plot->navigation();
    auto *toolbar_row = new QHBoxLayout;
    plot_box->addLayout(toolbar_row);
    toolbar_row->addWidget(navigation);
    auto *amplitude = new QLabel;
    amplitude->setObjectName("spectrum_amplitude_label");
    toolbar_row->addWidget(amplitude);
    toolbar_row->addStretch();
    auto *calculate = new QToolButton;
    calculate->setObjectName("spectrum_calculate");
    calculate->setIcon(ui_icon(UiIcon::play));
    calculate->setToolTip(text("spectrum_calculate"));
    calculate->setAccessibleName(text("spectrum_calculate"));
    calculate->setIconSize({24, 24});
    toolbar_row->addWidget(calculate);
    auto *export_button = new QToolButton;
    export_button->setIcon(ui_icon(UiIcon::export_data));
    export_button->setToolTip(text("spectrum_export"));
    export_button->setAccessibleName(text("spectrum_export"));
    export_button->setIconSize({24, 24});
    export_button->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(export_button);
    export_button->setMenu(menu);
    auto *csv = menu->addAction("CSV");
    auto *png = menu->addAction("PNG");
    toolbar_row->addWidget(export_button);
    plot_box->addWidget(plot, 1);
    splitter->addWidget(plot_page);
    auto *table = new QTableWidget(0, 5);
    table->setObjectName("spectrum_harmonic_table");
    table->setHorizontalHeaderLabels({text("spectrum_order"), "Hz", "RMS", "% H1", text("spectrum_phase")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    splitter->addWidget(table);
    splitter->setSizes({420, 140});
    auto *hint = new QLabel(text("spectrum_hint"));
    hint->setWordWrap(true);
    box->addWidget(hint);
    struct Data {
        std::optional<Spectrum> spectrum;
        Result plot;
    };
    auto spectrum_data = std::make_shared<Data>();
    auto compute = [=, this] {
        try {
            if (!result_ || result_->samples.empty())
                throw std::runtime_error(text("scope_empty").toStdString());
            int selected = -1;
            for (int ch : channels_)
                if (result_channel(*result_, ch).object == signal->currentData().toString().toStdString())
                    selected = ch;
            if (selected < 0)
                throw std::runtime_error(text("spectrum_missing_signal").toStdString());
            SpectrumOptions options;
            options.begin = parse_si(from->text().toStdString(), "s");
            options.end = parse_si(to->text().toStdString(), "s");
            options.fundamental = parse_si(frequency->text().toStdString(), "Hz");
            options.samples = size_t(size->currentData().toInt());
            options.harmonics = unsigned(count->value());
            options.window = SpectrumWindow(window->currentIndex());
            options.whole_periods = whole->isChecked();
            spectrum_data->spectrum = signal_spectrum(*result_, selected, options);
            const auto &s = *spectrum_data->spectrum;
            spectrum_data->plot = {};
            spectrum_data->plot.channels = {s.channel};
            spectrum_data->plot.samples.reserve(s.bins.size());
            for (const auto &bin : s.bins)
                spectrum_data->plot.samples.push_back({bin.frequency, {bin.amplitude}, {}});
            Project view;
            view.scope_end = s.sample_rate / 2;
            plot->set_result(&spectrum_data->plot, {0}, view);
            plot->fit();
            amplitude->setText(text("spectrum_amplitude").arg(QString::fromStdString(s.channel.unit)));
            table->setRowCount(int(s.harmonics.size()));
            for (int row = 0; row < int(s.harmonics.size()); ++row) {
                const auto &h = s.harmonics[row];
                const auto percent = s.harmonics.front().rms > 0
                                         ? QString::number(h.rms / s.harmonics.front().rms * 100, 'g', 7)
                                         : QString("—");
                const QStringList values{QString::number(h.order), QString::number(h.frequency, 'g', 10),
                                         engineering_value(h.rms, s.channel.unit), percent,
                                         QString::number(h.phase * 180 / std::numbers::pi, 'g', 7)};
                for (int col = 0; col < values.size(); ++col)
                    table->setItem(row, col, new QTableWidgetItem(values[col]));
            }
            const QString thd =
                s.thd ? QString::number(*s.thd * 100, 'g', 8) + " %" : text("spectrum_thd_unavailable");
            status->setText(
                text("spectrum_summary")
                    .arg(s.begin, 0, 'g', 8)
                    .arg(s.end, 0, 'g', 8)
                    .arg(engineering_value(s.sample_rate, "Hz"), engineering_value(s.resolution, "Hz"), thd)
                    .arg(s.harmonics.size()));
            status->setToolTip(text("spectrum_sampling")
                                   .arg(s.recorded_samples)
                                   .arg(engineering_value(s.maximum_interpolation_gap, "s")));
            export_button->setEnabled(true);
        } catch (const std::exception &e) {
            spectrum_data->spectrum.reset();
            spectrum_data->plot = {};
            plot->set_result(nullptr, {}, {});
            table->setRowCount(0);
            const auto *diagnostic = dynamic_cast<const Diagnostic *>(&e);
            status->setText(diagnostic ? text(diagnostic->code.c_str()) : QString::fromUtf8(e.what()));
            status->setToolTip({});
            export_button->setEnabled(false);
        }
    };
    connect(calculate, &QToolButton::clicked, dialog, compute);
    connect(csv, &QAction::triggered, dialog, [=] {
        if (!spectrum_data->spectrum)
            return;
        auto path = QFileDialog::getSaveFileName(dialog, text("spectrum_export"), {}, "CSV (*.csv)");
        if (path.isEmpty())
            return;
        std::ostringstream stream;
        write_spectrum_csv(stream, *spectrum_data->spectrum);
        const auto content = stream.str();
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(content.data(), qint64(content.size())) != qint64(content.size()) || !file.commit())
            status->setText(file.errorString());
    });
    connect(png, &QAction::triggered, dialog, [=] {
        auto path = QFileDialog::getSaveFileName(dialog, text("export_image"), {}, "PNG (*.png)");
        if (!path.isEmpty() && !plot->grab().save(path, "PNG"))
            status->setText(text("image_write_error"));
    });
    dialog->show();
    compute();
}
} // namespace pds::desktop
