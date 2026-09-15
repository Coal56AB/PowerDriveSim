#include "apps/desktop/editor.hpp"
#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QTimer>
int main(int argc, char **argv) {
    QApplication app(argc, argv);
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
        if (!parser.isSet(smoke))
            QMessageBox::critical(nullptr, "PowerDriveSim", QString::fromUtf8(e.what()));
        return 1;
    }
}
