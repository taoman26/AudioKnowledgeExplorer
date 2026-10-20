#pragma once
#include <QColor>
#include <QPalette>

namespace ake {

// Design tokens from design.md section 13. Light and dark variants.
struct Theme {
    QColor bg, panel, side, line, text, sub, faint, accent, accentBg, chip, chipText, green, amber, red, hl;

    static Theme light();
    static Theme dark();
    // Follows the system palette (Qt 6.2 has no colorScheme API).
    static Theme forPalette(const QPalette& p);

    // Stylesheet for the few widgets that need token colors; everything else inherits QPalette.
    QString styleSheet() const;
};

} // namespace ake
