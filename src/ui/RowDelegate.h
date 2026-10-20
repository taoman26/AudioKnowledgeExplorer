#pragma once
#include <QStyledItemDelegate>

#include "ui/CardDelegate.h"
#include "ui/Theme.h"

namespace ake {

// One recording per row (design.md section 10, mock-up 04): status dot | title + one line of summary | tags | date |
// duration | action. Dense, for scanning 100+ recordings. Uses the same model roles and actions as the cards.
class RowDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    static constexpr int kRowHeight = 62;
    static constexpr int kHeaderHeight = 32;
    static constexpr int kTagsMinWidth = 880;   // narrower than this the tags column is dropped

    // Horizontal layout for a row of `width` px; shared by the rows and the header so labels line up.
    struct Columns {
        int dotX = 22;
        int textX = 40, textW = 0;
        bool showTags = false;
        int tagsX = 0, tagsW = 0;
        int dateX = 0, dateW = 0;
        int durX = 0, durW = 0;     // right aligned inside [durX, durX+durW)
        int actionX = 0, actionW = 0;
    };
    static Columns columns(int width);

    explicit RowDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent), m_theme(Theme::light()) {}
    void setTheme(const Theme& t) { m_theme = t; }
    const Theme& theme() const { return m_theme; }
    void setWidth(int w) { m_width = w; }            // the viewport width: rows span it completely

    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return { m_width, kRowHeight }; }

    // The clickable button of a row's action (play on done rows, retry / analyze on the others).
    static QRect actionButtonRect(const QRect& itemRect, CardAction a);

private:
    Theme m_theme;
    int m_width = 800;
};

} // namespace ake
