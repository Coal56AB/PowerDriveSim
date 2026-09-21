#include "apps/desktop/editor.hpp"
#include "apps/desktop/safe_application.hpp"
#include <QCommandLineParser>
#include <QMessageBox>
#include <QDebug>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>
int main(int argc, char **argv) {
    pds::desktop::SafeApplication app(argc, argv);
    bool smoke_mode = false;
    try {
        QCoreApplication::setOrganizationName("PowerDriveSim");
        QCoreApplication::setApplicationName("PowerDriveSim");
        QCommandLineParser parser;
        parser.setApplicationDescription("PowerDriveSim — power electronics simulator");
        parser.addHelpOption();
        QCommandLineOption smoke("smoke-test", "Check application startup and exit automatically");
        smoke.setFlags(QCommandLineOption::HiddenFromHelp);
        parser.addOption(smoke);
        parser.addOption({"lang", "Interface language: ru or en", "language", "ru"});
        parser.addPositionalArgument("project", "Project file (.pds)");
        parser.process(app);
        smoke_mode = parser.isSet(smoke);
        QTemporaryDir smoke_dir;
        pds::desktop::EditorWindow window(parser.value("lang"),
                                          smoke_mode ? smoke_dir.path() : QString());
        app.set_warning_handler([&window](const QString &message) { window.report_unhandled_error(message); });
        app.setWindowIcon(QIcon(":/icons/application.png"));
        window.setWindowIcon(app.windowIcon());
        if (!parser.positionalArguments().empty()) {
            bool loaded = window.open_project(parser.positionalArguments().front());
            if (smoke_mode && !loaded)
                return 1;
        }
        if (smoke_mode)
            QTimer::singleShot(250, &app, &QCoreApplication::quit);
        window.show();
        return app.exec();
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        qWarning().noquote() << QString::fromUtf8(e.what());
        if (!smoke_mode)
            QMessageBox::critical(nullptr, "PowerDriveSim", QString::fromUtf8(e.what()));
        return 1;
    } catch (...) {
        const QString message = QStringLiteral("Unexpected internal error");
        std::cerr << message.toStdString() << std::endl;
        qWarning().noquote() << message;
        if (!smoke_mode)
            QMessageBox::warning(nullptr, "PowerDriveSim", message);
        return 1;
    }
}
