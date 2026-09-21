#pragma once

#include <QApplication>
#include <QDebug>
#include <QEvent>
#include <QStringList>
#include <QTimer>
#include <exception>
#include <functional>
#include <utility>

namespace pds::desktop {

// Qt does not permit C++ exceptions to escape an event handler. Keep a final
// process-wide boundary around event dispatch so an unexpected recoverable
// failure is reported in the UI instead of terminating the desktop process.
class SafeApplication final : public QApplication {
  public:
    using WarningHandler = std::function<void(const QString &)>;

    SafeApplication(int &argc, char **argv) : QApplication(argc, argv) {}

    void set_warning_handler(WarningHandler handler) { warning_handler_ = std::move(handler); }

  protected:
    bool notify(QObject *receiver, QEvent *event) override {
        try {
            return QApplication::notify(receiver, event);
        } catch (const std::exception &error) {
            queue_warning(QString::fromUtf8(error.what()));
        } catch (...) {
            queue_warning(QStringLiteral("Unknown error in the user interface"));
        }
        return false;
    }

  private:
    WarningHandler warning_handler_;
    QStringList pending_warnings_;
    bool warning_scheduled_ = false;

    void queue_warning(const QString &message) {
        qCritical().noquote() << message;
        try {
            if (!pending_warnings_.contains(message) && pending_warnings_.size() < 8)
                pending_warnings_.push_back(message);
            if (warning_scheduled_)
                return;
            warning_scheduled_ = true;
            QTimer::singleShot(0, this, [this] {
                warning_scheduled_ = false;
                const auto warnings = std::exchange(pending_warnings_, {});
                for (const auto &warning : warnings) {
                    try {
                        if (warning_handler_)
                            warning_handler_(warning);
                    } catch (const std::exception &reporting_error) {
                        qCritical().noquote() << reporting_error.what();
                    } catch (...) {
                        qCritical() << "Failed to report an unexpected UI error";
                    }
                }
            });
        } catch (...) {
            // Logging above is the allocation-free fallback available here.
        }
    }
};

} // namespace pds::desktop
