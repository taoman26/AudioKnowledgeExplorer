#include "ui/RowDelegate.h"

#include <QPainter>

#include "ui/RecordingListModel.h"

namespace ake {

namespace {
constexpr int kMarginRight = 14;
constexpr int kActionW = 84;
constexpr int kDurW = 64;
constexpr int kDateW = 140;
constexpr int kTagsW = 230;
constexpr int kGap = 12;

QFont px(const QFont& base, int pixels, bool bold = false)
{
    QFont f = base;
    f.setPixelSize(pixels);
    f.setBold(bold);
    return f;
}
} // namespace

RowDelegate::Columns RowDelegate::columns(int width)
{
    Columns c;
    c.actionW = kActionW;
    c.actionX = width - kMarginRight - kActionW;
    c.durW = kDurW;
    c.durX = c.actionX - kGap - kDurW;
    c.dateW = kDateW;
    c.dateX = c.durX - kGap - kDateW;
    c.showTags = width >= kTagsMinWidth;
    if (c.showTags) {
        c.tagsW = kTagsW;
        c.tagsX = c.dateX - kGap - kTagsW;
    }
    const int rightOfText = (c.showTags ? c.tagsX : c.dateX) - 16;
    c.textW = qMax(80, rightOfText - c.textX);
    return c;
}

QRect RowDelegate::actionButtonRect(const QRect& r, CardAction a)
{
    const Columns c = columns(r.width());
    const int right = r.left() + c.actionX + c.actionW;
    switch (a) {
    case CardAction::Play: return QRect(right - 28, r.top() + (kRowHeight - 22) / 2, 28, 22);
    case CardAction::Retry: return QRect(right - 70, r.top() + (kRowHeight - 24) / 2, 70, 24);
    case CardAction::Analyze: return QRect(right - 80, r.top() + (kRowHeight - 24) / 2, 80, 24);
    }
    return {};
}

void RowDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const
{
    const Theme& t = m_theme;
    const CardState state = idx.data(RecordingListModel::StateRole).value<CardState>();
    const bool selected = opt.state & QStyle::State_Selected;
    const bool hover = opt.state & QStyle::State_MouseOver;
    const bool playing = idx.data(RecordingListModel::PlayingRole).toBool();
    const QRect r = opt.rect;
    const Columns c = columns(r.width());
    const QFont base = opt.font;

    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    if (selected) p->fillRect(r, t.accentBg);
    else if (hover) { QColor h = t.line; h.setAlpha(70); p->fillRect(r, h); }
    QColor sep = t.line; sep.setAlpha(120);
    p->setPen(sep);
    p->drawLine(r.left(), r.bottom(), r.right(), r.bottom());

    // status dot
    const QColor dot = state == CardState::Done ? t.green : state == CardState::Running ? t.accent
                       : state == CardState::Error ? t.red : t.faint;
    p->setPen(Qt::NoPen);
    p->setBrush(dot);
    p->drawEllipse(QPoint(r.left() + c.dotX, r.top() + 22), 5, 5);

    // title (+ favourite star)
    const int textLeft = r.left() + c.textX;
    p->setFont(px(base, 14, true));
    p->setPen(state == CardState::Done ? t.text : t.sub);
    const bool fav = idx.data(RecordingListModel::FavoriteRole).toBool();
    const int titleW = c.textW - (fav ? 20 : 0);
    const QString title = p->fontMetrics().elidedText(idx.data(RecordingListModel::TitleRole).toString(), Qt::ElideRight, titleW);
    p->drawText(QRect(textLeft, r.top() + 9, titleW, 22), Qt::AlignLeft | Qt::AlignVCenter, title);
    if (fav) {
        p->setPen(t.amber);
        p->drawText(QRect(textLeft + p->fontMetrics().horizontalAdvance(title) + 6, r.top() + 9, 16, 22), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("★"));
    }

    // second line: summary / search snippet / state
    const QFont small = px(base, 12);
    const QFontMetrics sfm(small);
    p->setFont(small);
    const int lineTop = r.top() + 33;
    const QString snippet = idx.data(RecordingListModel::SnippetRole).toString();
    const bool searching = state == CardState::Done && !snippet.isEmpty();
    switch (state) {
    case CardState::Done: {
        const auto lines = CardDelegate::wrapElideLines(searching ? snippet : idx.data(RecordingListModel::SummaryRole).toString(), sfm, c.textW, 1);
        if (lines.isEmpty()) break;
        const int baseline = lineTop + (20 + sfm.ascent() - sfm.descent()) / 2;
        const QVariantList hl = searching ? idx.data(RecordingListModel::HighlightsRole).toList() : QVariantList();
        for (int k = 0; k + 1 < hl.size(); k += 2) {                       // search matches
            const int from = qMax(hl[k].toInt(), lines[0].start), to = qMin(hl[k].toInt() + hl[k + 1].toInt(), lines[0].start + int(lines[0].text.size()));
            if (to <= from) continue;
            p->setPen(Qt::NoPen);
            p->setBrush(t.hl);
            p->drawRoundedRect(QRect(textLeft + sfm.horizontalAdvance(lines[0].text.left(from - lines[0].start)) - 1, baseline - sfm.ascent(),
                                     sfm.horizontalAdvance(lines[0].text.mid(from - lines[0].start, to - from)) + 2, sfm.height()), 2, 2);
        }
        p->setPen(t.sub);
        p->drawText(QPoint(textLeft, baseline), lines[0].text);
        break;
    }
    case CardState::Running: {
        const double prog = idx.data(RecordingListModel::ProgressRole).toDouble();
        const QString stage = idx.data(RecordingListModel::StageRole).toString();
        const QString what = stage == "decode" ? QStringLiteral("デコード中") : stage == "load" ? QStringLiteral("モデル読み込み中") : QStringLiteral("文字起こし中");
        p->setPen(t.accent);
        p->drawText(QRect(textLeft, lineTop, c.textW, 20), Qt::AlignLeft | Qt::AlignVCenter,
                    prog >= 0 ? QStringLiteral("%1… %2%").arg(what).arg(int(prog * 100 + 0.5)) : QStringLiteral("%1…").arg(what));
        break;
    }
    case CardState::Error:
        p->setPen(t.red);
        p->drawText(QRect(textLeft, lineTop, c.textW, 20), Qt::AlignLeft | Qt::AlignVCenter,
                    sfm.elidedText(idx.data(RecordingListModel::ErrorRole).toString().isEmpty() ? QStringLiteral("解析に失敗しました")
                                                                                                : idx.data(RecordingListModel::ErrorRole).toString(), Qt::ElideRight, c.textW));
        break;
    case CardState::Pending:
        p->setPen(t.faint);
        p->drawText(QRect(textLeft, lineTop, c.textW, 20), Qt::AlignLeft | Qt::AlignVCenter,
                    idx.data(RecordingListModel::StatusRole).toString() == "queued" ? QStringLiteral("待機中…") : QStringLiteral("未解析"));
        break;
    }

    // tags (and the search badge)
    if (c.showTags && state == CardState::Done) {
        const QFont chipFont = px(base, 11);
        const QFontMetrics cfm(chipFont);
        p->setFont(chipFont);
        int cx = r.left() + c.tagsX;
        const int cy = r.top() + (kRowHeight - 20) / 2, ch = 20, limit = r.left() + c.tagsX + c.tagsW;
        const int hits = idx.data(RecordingListModel::HitCountRole).toInt();
        const bool similar = idx.data(RecordingListModel::SimilarRole).toBool();
        if (searching && (hits > 0 || similar)) {
            const QString badge = similar ? QStringLiteral("読みが近い") : QStringLiteral("%1箇所一致").arg(hits);
            const int bw = cfm.horizontalAdvance(badge) + 14;
            p->setPen(Qt::NoPen); p->setBrush(t.hl);
            p->drawRoundedRect(QRect(cx, cy, bw, ch), ch / 2, ch / 2);
            p->setPen(t.text);
            p->drawText(QRect(cx, cy, bw, ch), Qt::AlignCenter, badge);
            cx += bw + 6;
        }
        const QStringList kws = idx.data(RecordingListModel::KeywordsRole).toStringList();
        int shown = 0;
        for (const QString& k : kws) {
            const int cw = cfm.horizontalAdvance(k) + 16;
            if (cx + cw > limit - 26) break;                 // leave room for "+N"
            p->setPen(Qt::NoPen); p->setBrush(t.chip);
            p->drawRoundedRect(QRect(cx, cy, cw, ch), ch / 2, ch / 2);
            p->setPen(t.chipText);
            p->drawText(QRect(cx, cy, cw, ch), Qt::AlignCenter, k);
            cx += cw + 6;
            ++shown;
        }
        if (shown < kws.size()) {
            p->setPen(t.faint);
            p->drawText(QRect(cx, cy, 30, ch), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("+%1").arg(kws.size() - shown));
        }
    }

    // date, duration
    p->setFont(px(base, 12));
    p->setPen(t.sub);
    p->drawText(QRect(r.left() + c.dateX, r.top(), c.dateW, kRowHeight), Qt::AlignLeft | Qt::AlignVCenter,
                idx.data(RecordingListModel::RecordedAtRole).toDateTime().toString("yyyy-MM-dd HH:mm"));
    p->drawText(QRect(r.left() + c.durX, r.top(), c.durW, kRowHeight), Qt::AlignRight | Qt::AlignVCenter,
                CardDelegate::formatDuration(idx.data(RecordingListModel::DurationMsRole).toLongLong()));

    // action: play (hover / selected / playing), or retry / analyze
    CardAction a;
    if (CardDelegate::actionFor(idx, &a)) {
        const bool showButton = a != CardAction::Play || hover || selected || playing;
        if (showButton) {
            const QRect b = actionButtonRect(r, a);
            if (a == CardAction::Play) {
                p->setPen(Qt::NoPen);
                p->setBrush(t.accent);
                p->drawRoundedRect(b, 11, 11);
                p->setBrush(Qt::white);
                const QPointF m = b.center();
                if (playing) { p->drawRect(QRectF(m.x() - 4, m.y() - 5, 3, 10)); p->drawRect(QRectF(m.x() + 1, m.y() - 5, 3, 10)); }
                else p->drawPolygon(QPolygonF({ QPointF(m.x() - 3, m.y() - 5), QPointF(m.x() - 3, m.y() + 5), QPointF(m.x() + 5, m.y()) }));
            } else {
                p->setBrush(t.panel);
                p->setPen(t.line);
                p->drawRoundedRect(b, 5, 5);
                p->setPen(t.text);
                p->setFont(px(base, 11));
                p->drawText(b, Qt::AlignCenter, a == CardAction::Retry ? QStringLiteral("再試行") : QStringLiteral("解析する"));
            }
        }
    }
    p->restore();
}

} // namespace ake
