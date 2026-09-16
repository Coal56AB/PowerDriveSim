#include "apps/desktop/editor.hpp"
#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QStandardPaths>

namespace pds::desktop {
bool bundled_example(const QString &path) {
    const auto directory = QFileInfo(path).absoluteDir().canonicalPath();
    const QDir app(QCoreApplication::applicationDirPath());
    return !directory.isEmpty() && (directory == QDir(app.filePath("examples")).canonicalPath() ||
                                    directory == QDir(app.filePath("../examples")).canonicalPath() ||
                                    directory == QDir(app.filePath("../../examples")).canonicalPath());
}
QString EditorWindow::suggested_save_path() const {
    if (!path_.isEmpty())
        return path_;
    if (example_origin_.isEmpty())
        return {};
    return QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
        .filePath(QFileInfo(example_origin_).completeBaseName() + "-copy.pds");
}
void EditorWindow::build_examples_menu(QMenu *menu) {
    menu->setObjectName("examples_menu");
    const QDir directory(QCoreApplication::applicationDirPath() + "/examples");
    std::map<QString, QMenu *> groups;
    const std::map<QString, QString> categories{{"rc", "examples_basics"},
                                                {"rc-trapezoidal", "examples_basics"},
                                                {"rlc", "examples_basics"},
                                                {"switch", "examples_basics"},
                                                {"rc-sine", "examples_sources"},
                                                {"rc-pulse", "examples_sources"},
                                                {"rc-table", "examples_sources"},
                                                {"rc-sweep", "examples_experiments"},
                                                {"diode-freewheel", "examples_devices"},
                                                {"diode-recovery", "examples_devices"},
                                                {"thyristor-halfwave", "examples_devices"},
                                                {"diode-bridge-1p", "converters/rectifiers"},
                                                {"diode-bridge-3p", "converters/rectifiers"},
                                                {"thyristor-bridge-1p", "converters/rectifiers"},
                                                {"thyristor-bridge-3p", "converters/rectifiers"},
                                                {"buck", "converters/dc_dc"},
                                                {"boost", "converters/dc_dc"},
                                                {"buck-boost", "converters/dc_dc"},
                                                {"bidirectional-charge", "converters/dc_dc"},
                                                {"bidirectional-discharge", "converters/dc_dc"},
                                                {"half-bridge", "converters/inverters"},
                                                {"full-bridge", "converters/inverters"},
                                                {"vsi-2l", "converters/inverters"},
                                                {"npc-3l", "converters/inverters"},
                                                {"open-end-winding", "converters/inverters"},
                                                {"ac-voltage-controller", "converters/ac_ac"},
                                                {"precharge-discharge", "converters/dc_bus"},
                                                {"braking-chopper", "converters/dc_bus"}};
    for (const auto &file : directory.entryList({"*.pds"}, QDir::Files)) {
        const auto base = QFileInfo(file).completeBaseName();
        const auto found = categories.find(base);
        const auto category = found == categories.end() ? QString("examples_other") : found->second;
        QMenu *parent = menu;
        QString key;
        for (const auto &part : category.split('/')) {
            key += '/' + part;
            auto *&group = groups[key];
            if (!group)
                group = parent->addMenu(text(part.toUtf8().constData()));
            parent = group;
        }
        const auto label_key = "example_" + base;
        auto title = text(label_key.toUtf8().constData());
        if (title == label_key)
            title = base;
        auto *action = parent->addAction(title);
        action->setObjectName("example_" + base);
        action->setToolTip(file);
        connect(action, &QAction::triggered, this, [this, directory, file] {
            if (!running() && confirm_discard())
                open_project(directory.filePath(file));
        });
    }
}
} // namespace pds::desktop
