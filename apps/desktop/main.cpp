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
    QTemporaryDir smoke_dir;
    try {
        pds::desktop::EditorWindow window(parser.value("lang"),
                                          parser.isSet(smoke) ? smoke_dir.path() : QString());
        app.set_warning_handler([&window](const QString &message) { window.report_unhandled_error(message); });
        app.setWindowIcon(QIcon(":/icons/application.png"));
        window.setWindowIcon(app.windowIcon());
        if (!parser.positionalArguments().empty()) {
            bool loaded = window.open_project(parser.positionalArguments().front());
            if (parser.isSet(smoke) && !loaded)
                return 1;
        }
        if (parser.isSet(smoke))
            QTimer::singleShot(250, &app, &QCoreApplication::quit);
        window.show();
        return app.exec();
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        qWarning().noquote() << QString::fromUtf8(e.what());
        if (!parser.isSet(smoke))
            QMessageBox::critical(nullptr, "PowerDriveSim", QString::fromUtf8(e.what()));
        return 1;
    } catch (...) {
        const QString message = QStringLiteral("Unexpected internal error");
        std::cerr << message.toStdString() << std::endl;
        qWarning().noquote() << message;
        if (!parser.isSet(smoke))
            QMessageBox::warning(nullptr, "PowerDriveSim", message);
        return 1;
    }
}
