#pragma once

#include <QApplication>
#include <QByteArray>
#include <QTest>

inline bool qt_platform_was_selected(int argc, char **argv) {
    if (!qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        return true;
    for (int i = 1; i < argc; ++i) {
        const QByteArray argument(argv[i]);
        if (argument == "-platform" || argument.startsWith("-platform="))
            return true;
    }
    return false;
}

template <class TestCase> int run_qt_test(int argc, char **argv) {
    // A test executable may be started directly from Explorer or a terminal
    // without CTest's environment. Prefer the bundled headless platform in
    // that case; explicit native/DPI runs keep their requested platform.
    if (!qt_platform_was_selected(argc, argv))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    TestCase test_case;
    return QTest::qExec(&test_case, argc, argv);
}
