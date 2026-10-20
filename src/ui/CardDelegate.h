#pragma once
#include <QRect>
#include <QStyledItemDelegate>

#include "ui/Theme.h"

namespace ake {

enum class CardAction { Play, Retry, Analyze };

// Paints a knowledge card (design.md section 6). No per-card widgets: cheap with 100s of recordings.
class CardDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    static constexpr int kCardWidth = 294;
    static constexpr int kCardHeight = 160;
    static constexpr int kMargin = 2; // space around the card inside the item rect

    explicit CardDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent), m_theme(Theme::light()) {}

    void setTheme(const Theme& t) { m_theme = t; }
    const Theme& theme() const { return m_theme; }

    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override
    {
        return { kCardWidth + 2 * kMargin, kCardHeight + 2 * kMargin };
    }

    // The card rectangle inside an item rect, and the clickable button inside a card (if the state has one).
    static QRect cardRect(const QRect& itemRect) { return itemRect.adjusted(kMargin, kMargin, -kMargin, -kMargin); }
    static QRect actionButtonRect(const QRect& itemRect, CardAction a);
    // Which action does this card state offer? (Play only while hovered; see paint)
    static bool actionFor(const QModelIndex& idx, CardAction* out);

    static QString formatDuration(qint64 ms);
    static QStringList wrapElide(const QString& text, const QFontMetrics& fm, int width, int maxLines);
    // Same wrapping, but each line also tells where it starts in `text` (to place search highlights).
    struct Line { QString text; int start; };
    static QList<Line> wrapElideLines(const QString& text, const QFontMetrics& fm, int width, int maxLines);

private:
    Theme m_theme;
};

} // namespace ake
