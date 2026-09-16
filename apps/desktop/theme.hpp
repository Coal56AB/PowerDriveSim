#pragma once
#include <QColor>
#include <array>
namespace pds::desktop {
struct ThemeColors {
    QColor window, surface, canvas, text, muted, border, grid, selected, hover;
    QColor electrical, gate, signal, accent, error, gate_fill;
    std::array<QColor, 5> curves;
};
const ThemeColors &theme_colors();
bool dark_theme();
void apply_theme(bool dark);
// Cached electrical port/wire colors retain their domain when the theme changes.
QColor themed_signal(QColor color);
} // namespace pds::desktop
