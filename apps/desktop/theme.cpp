#include "apps/desktop/theme.hpp"
#include <QApplication>
#include <QPalette>
#include <QStyleFactory>
namespace pds::desktop {
namespace {
bool dark = false;
const ThemeColors light{
    "#f3f6fa",
    "#ffffff",
    "#f4f7fb",
    "#263c55",
    "#657c95",
    "#cfdceb",
    "#d9e2ed",
    "#dceafe",
    "#edf4ff",
    "#146cca",
    "#17866d",
    "#8c67c8",
    "#3a7fe0",
    "#b8394e",
    "#e6f5ed",
    {QColor("#146cca"), QColor("#c56819"), QColor("#17866d"), QColor("#935ad5"), QColor("#d04769")}};
const ThemeColors night{
    "#19212d",
    "#222d3b",
    "#18222f",
    "#dee7f3",
    "#a3b2c6",
    "#46576d",
    "#344357",
    "#304d73",
    "#2c3c51",
    "#64adff",
    "#60d5b6",
    "#ba99f3",
    "#72acff",
    "#ff8899",
    "#213e39",
    {QColor("#64adff"), QColor("#ffb269"), QColor("#60d5b6"), QColor("#c3a0ff"), QColor("#ff88aa")}};
} // namespace
const ThemeColors &theme_colors() {
    return dark ? night : light;
}
bool dark_theme() {
    return dark;
}
QColor themed_signal(QColor color) {
    if (color == light.electrical || color == night.electrical)
        return theme_colors().electrical;
    if (color == light.gate || color == night.gate)
        return theme_colors().gate;
    if (color == light.signal || color == night.signal)
        return theme_colors().signal;
    return color;
}
void apply_theme(bool value) {
    dark = value;
    if (!qApp)
        return;
    static bool styled = false;
    if (!styled) {
        QApplication::setStyle(QStyleFactory::create("Fusion"));
        styled = true;
    }
    const auto &c = theme_colors();
    QPalette palette;
    palette.setColor(QPalette::Window, c.window);
    palette.setColor(QPalette::WindowText, c.text);
    palette.setColor(QPalette::Base, c.surface);
    palette.setColor(QPalette::AlternateBase, c.canvas);
    palette.setColor(QPalette::Text, c.text);
    palette.setColor(QPalette::Button, c.surface);
    palette.setColor(QPalette::ButtonText, c.text);
    palette.setColor(QPalette::BrightText, c.error);
    palette.setColor(QPalette::ToolTipBase, c.surface);
    palette.setColor(QPalette::ToolTipText, c.text);
    palette.setColor(QPalette::Highlight, c.selected);
    palette.setColor(QPalette::HighlightedText, c.text);
    palette.setColor(QPalette::Link, c.accent);
    palette.setColor(QPalette::LinkVisited, c.signal);
    palette.setColor(QPalette::Mid, c.border);
    palette.setColor(QPalette::Light, c.hover);
    palette.setColor(QPalette::Dark, c.canvas);
    palette.setColor(QPalette::PlaceholderText, c.muted);
    for (auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, c.muted);
    QApplication::setPalette(palette);
    qApp->setStyleSheet(R"(
        QMainWindow,QDialog { background:palette(window); color:palette(text); }
        QLabel,QCheckBox,QRadioButton { color:palette(text); }
        QMenuBar { background:palette(base); padding:4px 12px; border-bottom:1px solid palette(mid); }
        QMenuBar::item { padding:5px 12px; } QMenuBar::item:selected { background:palette(highlight); border-radius:4px; }
        QMenu { background:palette(base); color:palette(text); border:1px solid palette(mid); }
        QMenu::item:selected { background:palette(highlight); }
        QToolTip { color:palette(text); background:palette(base); border:1px solid palette(mid); }
        QToolBar#controls { background:#182c47; padding:12px 16px; spacing:10px; border:0; }
        QToolBar#controls QLabel { color:#dce7f7; }
        QToolBar#controls QLineEdit,QToolBar#controls QComboBox { background:#29415f; color:white; border:1px solid #49617c; border-radius:5px; padding:6px 8px; }
        QToolBar#controls QComboBox QAbstractItemView { background:palette(base); color:palette(text); }
        QToolButton { padding:7px 12px; border:1px solid palette(mid); border-radius:5px; background:palette(button); color:palette(button-text); }
        QToolButton:hover { background:palette(light); border-color:palette(link); }
        QToolButton#run_button,QToolButton#stop_button { color:white; border:0; padding:5px 10px; font-weight:600; }
        QToolButton#run_button { background:#16a085; } QToolButton#run_button:hover { background:#118873; }
        QToolButton#stop_button { background:#cf3e3e; } QToolButton#stop_button:hover { background:#ad2929; }
        QToolButton:disabled,QPushButton:disabled { color:palette(placeholder-text); }
        QDockWidget::title { background:palette(window); color:palette(text); padding:9px 12px; font-weight:600; }
        QLineEdit,QPlainTextEdit,QComboBox,QSpinBox,QDoubleSpinBox { background:palette(base); color:palette(text); border:1px solid palette(mid); border-radius:5px; padding:7px; selection-background-color:palette(highlight); selection-color:palette(text); }
        QLineEdit:focus,QPlainTextEdit:focus { border-color:palette(link); }
        QTreeWidget,QListWidget,QTableWidget { background:palette(base); color:palette(text); border:0; outline:0; padding:4px; }
        QHeaderView::section { background:palette(window); color:palette(text); padding:5px; border:1px solid palette(mid); }
        QTreeWidget::item,QListWidget::item { min-height:28px; border-radius:4px; padding:2px 6px; }
        QTreeWidget::item:selected,QListWidget::item:selected { background:palette(highlight); color:palette(text); }
        QTreeWidget::item:hover,QListWidget::item:hover { background:palette(light); }
        QPushButton { background:palette(button); border:1px solid palette(mid); border-radius:5px; padding:8px 12px; color:palette(button-text); }
        QPushButton:hover { background:palette(light); }
        QPushButton#apply_properties { background:palette(highlight); border-color:palette(link); font-weight:600; }
        QTabWidget::pane { border:1px solid palette(mid); background:palette(base); }
        QTabBar::tab { background:palette(window); padding:10px 18px; border:0; color:palette(placeholder-text); }
        QTabBar::tab:selected { background:palette(base); color:palette(text); border-top:2px solid palette(link); }
        QTabWidget#workspace_tabs QTabBar::tab { padding:10px 8px; }
        QCheckBox { spacing:8px; padding:4px; } QCheckBox::indicator { width:15px; height:15px; }
        QStatusBar { background:palette(base); color:palette(placeholder-text); border-top:1px solid palette(mid); padding:4px 12px; }
    )");
}
} // namespace pds::desktop
