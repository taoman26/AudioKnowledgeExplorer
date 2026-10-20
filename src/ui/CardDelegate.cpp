#include "ui/CardDelegate.h"

#include <QPainter>
#include <QPainterPath>

#include "ui/RecordingListModel.h"

namespace ake {

namespace {
QFont px(const QFont& base, int pixels, bool bold = false)
{
    QFont f = base;
    f.setPixelSize(pixels);
    f.setBold(bold);
    return f;
}
}

QString CardDelegate::formatDuration(qint64 ms)
{
    if (ms <= 0) return QStringLiteral("--:--");
    const qint64 s = ms / 1000;
    const qint64 h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'))
                 : QStringLiteral("%1:%2").arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'));
}

QList<CardDelegate::Line> CardDelegate::wrapElideLines(const QString& text, const QFontMetrics& fm, int width, int maxLines)
{
    // CJK text may break between any two characters; Latin words are kept whole (break at the last space instead).
    // Widths are accumulated per character (linear). Summing advances ignores kerning, which only ever makes a line
    // look a little wider than it is, so a wrapped line never overflows `width`.
    QList<Line> lines;
    QString cur;
    int curStart = 0, curW = 0;
    auto isLatin = [](QChar c) { return c.unicode() < 0x80 && c.isLetterOrNumber(); };
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text[i];
        if (cur.isEmpty() && ch.isSpace()) continue;                 // never start a line with a space
        const int adv = fm.horizontalAdvance(ch);
        if (!cur.isEmpty() && curW + adv > width) {
            const int sp = cur.lastIndexOf(' ');
            if (sp > 0 && isLatin(ch) && isLatin(cur.back())) {      // inside a Latin word: move the word down
                lines << Line{ cur.left(sp), curStart };
                cur = cur.mid(sp + 1);
                curStart += sp + 1;
                curW = fm.horizontalAdvance(cur);
            } else {
                lines << Line{ cur.trimmed(), curStart };
                cur.clear();
                curW = 0;
                if (ch.isSpace()) continue;
            }
        }
        if (cur.isEmpty()) curStart = i;
        cur += ch;
        curW += adv;
    }
    if (!cur.trimmed().isEmpty())
        lines << Line{ cur.trimmed(), curStart };
    if (lines.size() > maxLines) {
        lines = lines.mid(0, maxLines);
        QString& last = lines.last().text;
        while (!last.isEmpty() && fm.horizontalAdvance(last + QChar(0x2026)) > width) last.chop(1);
        last += QChar(0x2026);
    }
    return lines;
}

QStringList CardDelegate::wrapElide(const QString& text, const QFontMetrics& fm, int width, int maxLines)
{
    QStringList out;
    for (const Line& l : wrapElideLines(text, fm, width, maxLines)) out << l.text;
    return out;
}

QRect CardDelegate::actionButtonRect(const QRect& itemRect, CardAction a)
{
    const QRect c = cardRect(itemRect);
    switch (a) {
    case CardAction::Play: return QRect(c.right() - 14 - 28 + 1, c.bottom() - 28, 28, 22);
    case CardAction::Retry: return QRect(c.left() + 14, c.top() + 92, 70, 24);
    case CardAction::Analyze: return QRect(c.left() + 14, c.top() + 92, 80, 24);
    }
    return {};
}

bool CardDelegate::actionFor(const QModelIndex& idx, CardAction* out)
{
    switch (idx.data(RecordingListModel::StateRole).value<CardState>()) {
    case CardState::Done: *out = CardAction::Play; return true;
    case CardState::Error: *out = CardAction::Retry; return true;
    case CardState::Pending: // already queued: nothing to ask for
        if (idx.data(RecordingListModel::StatusRole).toString() == "queued") return false;
        *out = CardAction::Analyze;
        return true;
    case CardState::Running: return false;
    }
    return false;
}

void CardDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const
{
    const Theme& t = m_theme;
    const CardState state = idx.data(RecordingListModel::StateRole).value<CardState>();
    const bool selected = opt.state & QStyle::State_Selected;
    const bool hover = opt.state & QStyle::State_MouseOver;
    const QRect c = cardRect(opt.rect);
    const QFont base = opt.font;

    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);

    // shadow + body
    QColor shadow = t.line; shadow.setAlpha(110);
    p->setPen(Qt::NoPen);
    p->setBrush(shadow);
    p->drawRoundedRect(c.translated(1, 2), 9, 9);
    p->setBrush(t.panel);
    const QColor border = state == CardState::Error ? t.red : selected ? t.accent : t.line;
    const qreal bw = (selected || state == CardState::Error) ? 2 : 1;
    p->setPen(QPen(border, bw));
    p->drawRoundedRect(QRectF(c).adjusted(bw / 2, bw / 2, -bw / 2, -bw / 2), 9, 9); // stroke stays inside the card

    const int x = c.left() + 14, w = c.width() - 28;

    // title
    p->setFont(px(base, 14, true));
    p->setPen(state == CardState::Done ? t.text : t.sub);
    const bool fav = idx.data(RecordingListModel::FavoriteRole).toBool();
    const int titleW = w - (fav ? 22 : 0);
    p->drawText(QRect(x, c.top() + 10, titleW, 22), Qt::AlignLeft | Qt::AlignVCenter,
                p->fontMetrics().elidedText(idx.data(RecordingListModel::TitleRole).toString(), Qt::ElideRight, titleW));
    if (fav) {
        p->setPen(t.amber);
        p->setFont(px(base, 14));
        p->drawText(QRect(c.right() - 14 - 18, c.top() + 10, 18, 22), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("★"));
    }

    // body by state
    p->setFont(px(base, 12));
    const QFontMetrics fm(p->font());
    switch (state) {
    case CardState::Done: {
        const QString snippet = idx.data(RecordingListModel::SnippetRole).toString();
        const bool searching = !snippet.isEmpty();
        // while searching, the text around the match replaces the summary, with the matches highlighted
        const QList<Line> lines = wrapElideLines(searching ? snippet : idx.data(RecordingListModel::SummaryRole).toString(), fm, w, 3);
        const QVariantList hl = searching ? idx.data(RecordingListModel::HighlightsRole).toList() : QVariantList();
        for (int i = 0; i < lines.size(); ++i) {
            const int baseline = c.top() + 46 + i * 18;
            for (int k = 0; k + 1 < hl.size(); k += 2) {
                const int from = qMax(hl[k].toInt(), lines[i].start), to = qMin(hl[k].toInt() + hl[k + 1].toInt(), lines[i].start + int(lines[i].text.size()));
                if (to <= from) continue;
                const int x0 = x + fm.horizontalAdvance(lines[i].text.left(from - lines[i].start));
                const int ww = fm.horizontalAdvance(lines[i].text.mid(from - lines[i].start, to - from));
                p->setPen(Qt::NoPen);
                p->setBrush(t.hl);
                p->drawRoundedRect(QRect(x0 - 1, baseline - fm.ascent(), ww + 2, fm.height()), 2, 2);
            }
            p->setPen(t.sub);
            p->drawText(QPoint(x, baseline), lines[i].text);
        }
        // keyword chips (max 3 + "+N")
        const QStringList kws = idx.data(RecordingListModel::KeywordsRole).toStringList();
        p->setFont(px(base, 11));
        const QFontMetrics cfm(p->font());
        int cx = x;
        const int cy = c.top() + 100, ch = 20;
        const int hits = idx.data(RecordingListModel::HitCountRole).toInt();
        const bool similar = idx.data(RecordingListModel::SimilarRole).toBool();
        if (searching && (hits > 0 || similar)) {                         // "3箇所一致"
            const QString badge = similar ? QStringLiteral("読みが近い") : QStringLiteral("%1箇所一致").arg(hits);
            const int bw = cfm.horizontalAdvance(badge) + 14;
            p->setPen(Qt::NoPen);
            p->setBrush(t.hl);
            p->drawRoundedRect(QRect(cx, cy, bw, ch), ch / 2, ch / 2);
            p->setPen(t.text);
            p->drawText(QRect(cx, cy, bw, ch), Qt::AlignCenter, badge);
            cx += bw + 6;
        }
        for (int i = 0; i < qMin<int>(3, kws.size()); ++i) {
            const int cw = cfm.horizontalAdvance(kws[i]) + 16;
            if (cx + cw > x + w) break;
            p->setPen(Qt::NoPen);
            p->setBrush(t.chip);
            p->drawRoundedRect(QRect(cx, cy, cw, ch), ch / 2, ch / 2);
            p->setPen(t.chipText);
            p->drawText(QRect(cx, cy, cw, ch), Qt::AlignCenter, kws[i]);
            cx += cw + 6;
        }
        if (kws.size() > 3) {
            p->setPen(t.faint);
            p->drawText(QRect(cx, cy, 30, ch), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("+%1").arg(kws.size() - 3));
        }
        break;
    }
    case CardState::Running: {
        // skeleton lines + progress
        p->setPen(Qt::NoPen);
        QColor sk = t.line; sk.setAlpha(150);
        p->setBrush(sk);
        for (int i = 0; i < 3; ++i)
            p->drawRoundedRect(QRect(x, c.top() + 46 + i * 16, int(w * (i == 0 ? 1.0 : i == 1 ? 0.7 : 0.5)), 8), 4, 4);
        const double prog = idx.data(RecordingListModel::ProgressRole).toDouble();
        p->setPen(t.accent);
        p->setFont(px(base, 12));
        const QString stage = idx.data(RecordingListModel::StageRole).toString();
        const QString what = stage == "decode" ? QStringLiteral("デコード中") : stage == "load" ? QStringLiteral("モデル読み込み中")
                                                                         : QStringLiteral("文字起こし中");
        p->drawText(QPoint(x, c.top() + 108),
                    prog >= 0 ? QStringLiteral("%1… %2%").arg(what).arg(int(prog * 100 + 0.5)) : QStringLiteral("%1…").arg(what));
        break;
    }
    case CardState::Error: {
        p->setPen(t.red);
        p->drawText(QPoint(x, c.top() + 50), QStringLiteral("解析に失敗しました"));
        p->setPen(t.sub);
        p->setFont(px(base, 11));
        const QStringList lines = wrapElide(idx.data(RecordingListModel::ErrorRole).toString(), QFontMetrics(p->font()), w, 2);
        for (int i = 0; i < lines.size(); ++i)
            p->drawText(QPoint(x, c.top() + 68 + i * 15), lines[i]);
        break;
    }
    case CardState::Pending: {
        p->setPen(t.faint);
        p->drawText(QPoint(x, c.top() + 50), idx.data(RecordingListModel::StatusRole).toString() == "queued"
                                                  ? QStringLiteral("待機中…") : QStringLiteral("未解析"));
        break;
    }
    }

    // ghost buttons for Error / Pending
    CardAction offered;
    if (actionFor(idx, &offered) && (state == CardState::Error || state == CardState::Pending)) {
        const CardAction a = offered;
        const QRect b = actionButtonRect(opt.rect, a);
        p->setBrush(t.panel);
        p->setPen(t.line);
        p->drawRoundedRect(b, 5, 5);
        p->setPen(t.text);
        p->setFont(px(base, 11));
        p->drawText(b, Qt::AlignCenter, state == CardState::Error ? QStringLiteral("再試行") : QStringLiteral("解析する"));
    }

    // footer: separator, date, duration, play button (hover/selected only)
    p->setPen(t.line);
    p->drawLine(x, c.bottom() - 34, x + w, c.bottom() - 34);
    p->setFont(px(base, 11));
    p->setPen(t.faint);
    const QDateTime when = idx.data(RecordingListModel::RecordedAtRole).toDateTime();
    p->drawText(QRect(x, c.bottom() - 30, w, 24), Qt::AlignLeft | Qt::AlignVCenter, when.toString("yyyy-MM-dd HH:mm"));
    const bool playing = idx.data(RecordingListModel::PlayingRole).toBool();
    const bool showPlay = state == CardState::Done && (hover || selected || playing);   // the playing card keeps its button
    const int durRight = showPlay ? c.right() - 14 - 34 : c.right() - 14;
    p->drawText(QRect(x, c.bottom() - 30, durRight - x, 24), Qt::AlignRight | Qt::AlignVCenter,
                formatDuration(idx.data(RecordingListModel::DurationMsRole).toLongLong()));
    if (showPlay) {
        const QRect b = actionButtonRect(opt.rect, CardAction::Play);
        p->setPen(Qt::NoPen);
        p->setBrush(t.accent);
        p->drawRoundedRect(b, 11, 11);
        p->setBrush(Qt::white);
        const QPointF m = b.center();
        if (playing)                                   // two bars: "pause"
            { p->drawRect(QRectF(m.x() - 4, m.y() - 5, 3, 10)); p->drawRect(QRectF(m.x() + 1, m.y() - 5, 3, 10)); }
        else
            p->drawPolygon(QPolygonF({ QPointF(m.x() - 3, m.y() - 5), QPointF(m.x() - 3, m.y() + 5), QPointF(m.x() + 5, m.y()) }));
    }
    p->restore();
}

} // namespace ake
