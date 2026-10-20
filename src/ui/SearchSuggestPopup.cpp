#include "ui/SearchSuggestPopup.h"

#include <QPainter>
#include <QStyledItemDelegate>

namespace ake {

namespace {
QString stamp(qint64 ms)
{
    const qint64 s = ms / 1000;
    return QStringLiteral("%1:%2:%3").arg(s / 3600, 2, 10, QChar('0')).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
}
} // namespace

class SearchSuggestPopup::Delegate : public QStyledItemDelegate {
public:
    explicit Delegate(QObject* parent) : QStyledItemDelegate(parent), theme(Theme::light()) {}
    Theme theme;

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& i) const override
    {
        switch (i.data(SearchSuggestPopup::KindRole).toInt()) {
        case SearchSuggestPopup::Header: return { 100, 26 };
        case SearchSuggestPopup::SegmentItem: return { 100, 46 };
        default: return { 100, 28 };
        }
    }
    void paint(QPainter* p, const QStyleOptionViewItem& o, const QModelIndex& i) const override
    {
        const int kind = i.data(SearchSuggestPopup::KindRole).toInt();
        const QRect r = o.rect;
        QFont f = o.font;
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        if (kind == SearchSuggestPopup::Header) {
            f.setPixelSize(11); f.setBold(true);
            p->setFont(f); p->setPen(theme.faint);
            p->drawText(r.adjusted(16, 8, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, i.data(SearchSuggestPopup::TextRole).toString());
            p->restore();
            return;
        }
        if (o.state & QStyle::State_Selected) {
            p->setPen(Qt::NoPen); p->setBrush(theme.accentBg);
            p->drawRoundedRect(r.adjusted(6, 1, -6, -1), 5, 5);
        } else if (o.state & QStyle::State_MouseOver) {
            QColor h = theme.line; h.setAlpha(90);
            p->setPen(Qt::NoPen); p->setBrush(h);
            p->drawRoundedRect(r.adjusted(6, 1, -6, -1), 5, 5);
        }
        f.setPixelSize(13);
        p->setFont(f);
        const QFontMetrics fm(f);
        const QString text = i.data(SearchSuggestPopup::TextRole).toString();
        if (kind == SearchSuggestPopup::SegmentItem) {
            // snippet with the matches highlighted, and "recording title ・ time" below it
            const QVariantList hl = i.data(SearchSuggestPopup::HighlightsRole).toList();
            const int x = r.left() + 16, baseline = r.top() + 18;
            const QString shown = fm.elidedText(text, Qt::ElideRight, r.width() - 32);
            for (int k = 0; k + 1 < hl.size(); k += 2) {
                const int from = hl[k].toInt(), to = qMin(hl[k].toInt() + hl[k + 1].toInt(), int(shown.size()));
                if (to <= from) continue;
                p->setPen(Qt::NoPen); p->setBrush(theme.hl);
                p->drawRoundedRect(QRect(x + fm.horizontalAdvance(shown.left(from)) - 1, baseline - fm.ascent(),
                                         fm.horizontalAdvance(shown.mid(from, to - from)) + 2, fm.height()), 2, 2);
            }
            p->setPen(theme.text);
            p->drawText(QPoint(x, baseline), shown);
            f.setPixelSize(11);
            p->setFont(f); p->setPen(theme.faint);
            p->drawText(QRect(x, r.top() + 24, r.width() - 32, 18), Qt::AlignLeft | Qt::AlignVCenter,
                        QFontMetrics(f).elidedText(i.data(SearchSuggestPopup::SubTextRole).toString(), Qt::ElideRight, r.width() - 32));
        } else {
            p->setPen(theme.sub);
            p->drawText(QRect(r.left() + 16, r.top(), 18, r.height()), Qt::AlignCenter, kind == SearchSuggestPopup::TagItem ? "#" : "●");
            p->setPen(theme.text);
            const QString sub = i.data(SearchSuggestPopup::SubTextRole).toString();
            const int subW = sub.isEmpty() ? 0 : fm.horizontalAdvance(sub) + 12;
            p->drawText(QRect(r.left() + 40, r.top(), r.width() - 56 - subW, r.height()), Qt::AlignLeft | Qt::AlignVCenter,
                        fm.elidedText(text, Qt::ElideRight, r.width() - 56 - subW));
            if (!sub.isEmpty()) {
                p->setPen(theme.faint);
                p->drawText(QRect(r.right() - 16 - subW, r.top(), subW, r.height()), Qt::AlignRight | Qt::AlignVCenter, sub);
            }
        }
        p->restore();
    }
};

SearchSuggestPopup::SearchSuggestPopup(QWidget* parent)
    : QListWidget(parent), m_delegate(new Delegate(this))
{
    setObjectName("searchPopup");
    setItemDelegate(m_delegate);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::Box);
    hide();
    connect(this, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { activate(it); });
}

void SearchSuggestPopup::setTheme(const Theme& t)
{
    m_delegate->theme = t;
    viewport()->update();
}

void SearchSuggestPopup::setSuggestions(const Suggestions& s)
{
    clear();
    auto add = [&](Kind k, const QString& text, qint64 id = 0, const QString& sub = QString(), qint64 ms = -1, const QVariantList& hl = {}) {
        auto* it = new QListWidgetItem(this);
        it->setData(KindRole, k); it->setData(TextRole, text); it->setData(IdRole, id);
        it->setData(SubTextRole, sub); it->setData(MsRole, ms); it->setData(HighlightsRole, hl);
        if (k == Header) it->setFlags(Qt::ItemIsEnabled); // not selectable
        return it;
    };
    if (!s.tags.isEmpty()) {
        add(Header, tr("タグ"));
        for (const auto& t : s.tags) add(TagItem, t.name, t.id, QString::number(t.count));
    }
    if (!s.recordings.isEmpty()) {
        add(Header, tr("録音"));
        for (const auto& r : s.recordings) add(RecordingItem, r.title, r.id);
    }
    if (!s.segments.isEmpty()) {
        add(Header, tr("全文（発言）"));
        for (const auto& g : s.segments) {
            QVariantList hl;
            for (int v : g.highlights) hl << v;
            add(SegmentItem, g.snippet, g.recordingId, QStringLiteral("%1 ・ %2").arg(g.recordingTitle, stamp(g.startMs)), g.startMs, hl);
        }
    }
    m_hasItems = count() > 0;
    setCurrentRow(-1);
    clearSelection();
}

int SearchSuggestPopup::itemCountOfKind(Kind k) const
{
    int n = 0;
    for (int i = 0; i < count(); ++i) n += item(i)->data(KindRole).toInt() == k;
    return n;
}

void SearchSuggestPopup::moveSelection(int delta)
{
    if (count() == 0) return;
    QList<int> rows;
    for (int i = 0; i < count(); ++i) if (item(i)->flags() & Qt::ItemIsSelectable) rows << i;
    if (rows.isEmpty()) return;
    int pos = hasSelection() ? int(rows.indexOf(currentRow())) : -1;
    pos = qBound(0, pos + delta, int(rows.size()) - 1);
    if (pos < 0) pos = 0;
    setCurrentRow(rows[pos]);
}

bool SearchSuggestPopup::activateCurrent()
{
    if (!hasSelection()) return false;
    activate(currentItem());
    return true;
}

void SearchSuggestPopup::activate(QListWidgetItem* it)
{
    if (!it) return;
    const qint64 id = it->data(IdRole).toLongLong();
    switch (it->data(KindRole).toInt()) {
    case TagItem: emit tagChosen(id, it->data(TextRole).toString()); break;
    case RecordingItem: emit recordingChosen(id); break;
    case SegmentItem: emit segmentChosen(id, it->data(MsRole).toLongLong()); break;
    default: break;
    }
}

int SearchSuggestPopup::preferredHeight() const
{
    int h = 8;
    for (int i = 0; i < count(); ++i) h += sizeHintForRow(i);
    return h;
}

} // namespace ake
