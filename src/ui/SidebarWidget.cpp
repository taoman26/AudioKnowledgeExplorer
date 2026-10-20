#include "ui/SidebarWidget.h"

#include <QFileInfo>
#include <QHeaderView>
#include <QPainter>
#include <QScrollBar>

namespace ake {

using Scope = RecordingFilter::Scope;
using Period = RecordingFilter::Period;

// ---------------------------------------------------------------- delegate
QSize SidebarDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex& idx) const
{
    return idx.data(SidebarWidget::KindRole).toInt() == SidebarWidget::Header ? QSize(100, 30) : QSize(100, 28);
}

void SidebarDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const
{
    const auto kind = idx.data(SidebarWidget::KindRole).toInt();
    const bool active = idx.data(SidebarWidget::ActiveRole).toBool();
    const bool hover = opt.state & QStyle::State_MouseOver;
    QFont f = opt.font;
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    const QRect r = opt.rect;

    if (kind == SidebarWidget::Header) {
        f.setPixelSize(11);
        f.setBold(true);
        p->setFont(f);
        p->setPen(m_theme.faint);
        p->drawText(r.adjusted(14, 8, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, idx.data(Qt::DisplayRole).toString());
        p->restore();
        return;
    }

    const QRect row = r.adjusted(6, 1, -6, -1);
    if (active) {
        p->setPen(Qt::NoPen);
        p->setBrush(m_theme.accentBg);
        p->drawRoundedRect(row, 5, 5);
    } else if (hover) {
        QColor h = m_theme.line; h.setAlpha(90);
        p->setPen(Qt::NoPen);
        p->setBrush(h);
        p->drawRoundedRect(row, 5, 5);
    }

    f.setPixelSize(13);
    f.setBold(active);
    p->setFont(f);
    const QString icon = idx.data(SidebarWidget::IconRole).toString();
    p->setPen(active ? m_theme.accent : m_theme.sub);
    p->drawText(QRect(row.left() + 10, row.top(), 20, row.height()), Qt::AlignCenter, icon);

    const int count = idx.data(SidebarWidget::CountRole).toInt();
    int right = row.right() - 10;
    if (count >= 0) {
        QFont cf = opt.font;
        cf.setPixelSize(11);
        p->setFont(cf);
        p->setPen(m_theme.faint);
        const QString c = QString::number(count);
        const int cw = QFontMetrics(cf).horizontalAdvance(c);
        p->drawText(QRect(right - cw, row.top(), cw, row.height()), Qt::AlignRight | Qt::AlignVCenter, c);
        right -= cw + 8;
    }
    p->setFont(f);
    const bool more = kind == SidebarWidget::MoreTags;
    p->setPen(more ? m_theme.accent : m_theme.text);
    const QString text = QFontMetrics(f).elidedText(idx.data(Qt::DisplayRole).toString(), Qt::ElideRight, right - (row.left() + 36));
    p->drawText(QRect(row.left() + 36, row.top(), right - (row.left() + 36), row.height()), Qt::AlignLeft | Qt::AlignVCenter, text);
    p->restore();
}

// ---------------------------------------------------------------- widget
SidebarWidget::SidebarWidget(QWidget* parent)
    : QTreeWidget(parent), m_delegate(new SidebarDelegate(this))
{
    setObjectName("sidebar");
    setHeaderHidden(true);
    setRootIsDecorated(false);
    setIndentation(0);
    setFrameShape(QFrame::NoFrame);
    setItemDelegate(m_delegate);
    setMouseTracking(true);
    setSelectionMode(QAbstractItemView::NoSelection);
    setFocusPolicy(Qt::StrongFocus);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(this, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* it) { activate(it); });
    connect(this, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* it) { activate(it); }); // Enter key
    setAttribute(Qt::WA_StyledBackground, true);
}

void SidebarWidget::setTheme(const Theme& t)
{
    m_delegate->setTheme(t);
    viewport()->update();
}

QString SidebarWidget::folderLabel(const QString& path)
{
    const QString name = QFileInfo(path).fileName();
    return name.isEmpty() ? path : name;
}

QTreeWidgetItem* SidebarWidget::add(Kind k, const QString& text, qint64 key, int count, const QString& icon)
{
    auto* it = new QTreeWidgetItem(this);
    it->setText(0, text);
    it->setData(0, KindRole, k);
    it->setData(0, KeyRole, key);
    it->setData(0, CountRole, count);
    it->setData(0, IconRole, icon);
    return it;
}

void SidebarWidget::addHeader(const QString& text)
{
    auto* it = new QTreeWidgetItem(this);
    it->setText(0, text);
    it->setData(0, KindRole, Header);
    it->setFlags(Qt::ItemIsEnabled); // not selectable/activatable
}

QTreeWidgetItem* SidebarWidget::findItem(Kind kind, qint64 key) const
{
    for (int i = 0; i < topLevelItemCount(); ++i) {
        QTreeWidgetItem* it = topLevelItem(i);
        if (it->data(0, KindRole).toInt() == kind && it->data(0, KeyRole).toLongLong() == key) return it;
    }
    return nullptr;
}

void SidebarWidget::setData(const SidebarData& d)
{
    m_data = d;
    // Drop selections that vanished (folder removed, tag no longer used, year gone).
    RecordingFilter f = m_filter;
    if (f.scope == Scope::Folder) {
        bool found = false;
        for (const auto& fo : d.folders) found |= fo.id == f.folderId;
        if (!found) { f.scope = Scope::All; f.folderId = 0; }
    }
    if (f.period == Period::Year) {
        bool found = false;
        for (const auto& y : d.years) found |= y.year == f.year;
        if (!found) { f.period = Period::None; f.year = 0; }
    }
    for (auto it = f.tagIds.begin(); it != f.tagIds.end();) {
        bool found = false;
        for (const auto& t : d.tags) found |= t.id == *it;
        it = found ? it + 1 : f.tagIds.erase(it);
    }
    const bool changed = !(f == m_filter);
    m_filter = f;
    rebuild();
    if (changed) emit filterChanged(m_filter);
}

void SidebarWidget::setFilter(const RecordingFilter& f)
{
    if (f == m_filter) return;
    m_filter = f;
    rebuild();
    emit filterChanged(m_filter);
}

void SidebarWidget::rebuild()
{
    const int scroll = verticalScrollBar()->value();
    clear();
    const SidebarData& d = m_data;
    auto lib = [&](const QString& label, Scope s, int n, const QString& icon) {
        auto* it = add(Library, label, int(s), n, icon);
        it->setData(0, ActiveRole, m_filter.scope == s);
    };
    addHeader(tr("ライブラリ"));
    lib(tr("すべて"), Scope::All, d.all, "▣");
    lib(tr("お気に入り"), Scope::Favorites, d.favorites, "★");
    lib(tr("最近追加"), Scope::Recent, d.recent, "◷");
    lib(tr("未解析"), Scope::Unanalyzed, d.unanalyzed, "◔");
    lib(tr("エラー"), Scope::Error, d.error, "⚠");

    addHeader(tr("フォルダ"));
    for (const auto& f : d.folders) {
        auto* it = add(Folder, folderLabel(f.path), f.id, f.count, "▰");
        it->setToolTip(0, f.path);
        it->setData(0, ActiveRole, m_filter.scope == Scope::Folder && m_filter.folderId == f.id);
    }

    addHeader(tr("タグ"));
    QList<qint64> shown;
    for (int i = 0; i < d.tags.size() && i < kTopTags; ++i) shown << d.tags[i].id;
    for (qint64 id : m_filter.tagIds)                      // keep active tags visible even if rare
        if (!shown.contains(id)) shown << id;
    for (qint64 id : shown)
        for (const auto& t : d.tags)
            if (t.id == id) {
                auto* it = add(Tag, t.name, t.id, t.count, "#");
                it->setData(0, ActiveRole, m_filter.tagIds.contains(t.id));
            }
    if (d.tags.size() > kTopTags) add(MoreTags, tr("すべてのタグ…"), 0, -1, "");

    addHeader(tr("期間"));
    auto per = [&](const QString& label, Period p, int year, int n) {
        auto* it = add(PeriodItem, label, p == Period::Year ? year : int(p), n, "▸");
        it->setData(0, ActiveRole, m_filter.period == p && (p != Period::Year || m_filter.year == year));
    };
    per(tr("今週"), Period::ThisWeek, 0, d.thisWeek);
    per(tr("今月"), Period::ThisMonth, 0, d.thisMonth);
    for (const auto& y : d.years) per(tr("%1年").arg(y.year), Period::Year, y.year, y.count);
    verticalScrollBar()->setValue(scroll);
}

void SidebarWidget::activate(QTreeWidgetItem* item)
{
    if (!item) return;
    const Kind k = Kind(item->data(0, KindRole).toInt());
    const qint64 key = item->data(0, KeyRole).toLongLong();
    RecordingFilter f = m_filter;
    switch (k) {
    case Header: return;
    case Library: f.scope = Scope(key); f.folderId = 0; break;
    case Folder: f.scope = Scope::Folder; f.folderId = key; break;
    case Tag:
        if (f.tagIds.contains(key)) f.tagIds.removeAll(key); else f.tagIds << key;
        break;
    case PeriodItem: {
        const bool isYear = key > 1000; // years (2026) vs. enum values (1,2)
        const Period p = isYear ? Period::Year : Period(key);
        const int y = isYear ? int(key) : 0;
        if (f.period == p && f.year == y) { f.period = Period::None; f.year = 0; } // click again clears
        else { f.period = p; f.year = y; }
        break;
    }
    case MoreTags: emit allTagsRequested(); return;
    }
    setFilter(f);
}

} // namespace ake
