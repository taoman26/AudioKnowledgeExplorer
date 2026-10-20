#include "ui/FlowLayout.h"

namespace ake {

FlowLayout::~FlowLayout()
{
    while (QLayoutItem* it = takeAt(0)) delete it;
}

QSize FlowLayout::minimumSize() const
{
    QSize s;
    for (QLayoutItem* it : m_items) s = s.expandedTo(it->minimumSize());
    const QMargins m = contentsMargins();
    return s + QSize(m.left() + m.right(), m.top() + m.bottom());
}

int FlowLayout::doLayout(const QRect& rect, bool testOnly) const
{
    const QMargins m = contentsMargins();
    const QRect eff = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
    int x = eff.x(), y = eff.y(), rowH = 0;
    for (QLayoutItem* it : m_items) {
        if (it->isEmpty()) continue;
        const QSize hint = it->sizeHint();
        if (x + hint.width() > eff.right() + 1 && rowH > 0) {   // wrap
            x = eff.x();
            y += rowH + m_v;
            rowH = 0;
        }
        if (!testOnly) it->setGeometry(QRect(QPoint(x, y), hint));
        x += hint.width() + m_h;
        rowH = qMax(rowH, hint.height());
    }
    return y + rowH - rect.y() + m.top() + m.bottom();
}

} // namespace ake
