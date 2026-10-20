#include "ui/FilterChipBar.h"

#include <QPushButton>

#include "ui/FlowLayout.h"

namespace ake {

FilterChipBar::FilterChipBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("filterChipBar");
    m_flow = new FlowLayout(this, 6, 6);
    m_flow->setContentsMargins(0, 0, 0, 0);
    m_clearAll = new QPushButton(tr("すべて解除"), this);
    m_clearAll->setObjectName("linkButton");
    m_clearAll->setCursor(Qt::PointingHandCursor);
    connect(m_clearAll, &QPushButton::clicked, this, &FilterChipBar::clearAllRequested);
    hide();
}

QList<QPushButton*> FilterChipBar::chipButtons() const
{
    QList<QPushButton*> l;
    for (QPushButton* b : findChildren<QPushButton*>("filterChip"))
        if (!b->isHidden()) l << b;                     // removed ones wait for deleteLater
    return l;
}

void FilterChipBar::setChips(const QList<Chip>& chips)
{
    if (chips.size() == m_chips.size()) {                         // unchanged: don't rebuild (keeps hover/focus)
        bool same = true;
        for (int i = 0; i < chips.size(); ++i)
            same &= chips[i].kind == m_chips[i].kind && chips[i].key == m_chips[i].key && chips[i].text == m_chips[i].text;
        if (same) return;
    }
    m_chips = chips;
    // take everything out; the "clear all" button is kept, the chips are deleted
    while (QLayoutItem* it = m_flow->takeAt(0)) {
        if (QWidget* w = it->widget(); w && w != m_clearAll) { w->hide(); w->deleteLater(); }   // may be the button being clicked: not now
        delete it;
    }
    for (const Chip& c : m_chips) {
        auto* b = new QPushButton(c.text + QStringLiteral("  ✕"), this);
        b->setObjectName("filterChip");
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tr("この条件を外す"));
        const int kind = c.kind;
        const qint64 key = c.key;
        connect(b, &QPushButton::clicked, this, [this, kind, key] { emit removeRequested(kind, key); });
        m_flow->addWidget(b);
        b->show();                                      // children added to a parent that is already shown start hidden
    }
    if (m_chips.size() >= 2) m_flow->addWidget(m_clearAll);
    m_clearAll->setVisible(m_chips.size() >= 2);
    setVisible(!m_chips.isEmpty());
    updateGeometry();
}

} // namespace ake
