#include "ui/TranscriptModel.h"

#include <QSet>
#include <algorithm>

#include "core/Database.h"
#include "core/ITokenizer.h"

namespace ake {

QString TranscriptModel::formatTime(qint64 ms)
{
    const qint64 s = qMax<qint64>(0, ms) / 1000;
    return QStringLiteral("%1:%2:%3").arg(s / 3600, 2, 10, QChar('0')).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
}

QVariant TranscriptModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) return {};
    const auto& r = m_rows[index.row()];
    switch (role) {
    case Qt::DisplayRole: return r.effective();
    case StartMsRole: return r.startMs;
    case EndMsRole: return r.endMs;
    case OriginalRole: return r.text;
    case EditedRole: return r.edited();
    case SegmentIdRole: return r.id;
    case HighlightsRole: {
        QVariantList l;
        for (int v : highlightsOf(index.row())) l << v;
        return l;
    }
    }
    return {};
}

void TranscriptModel::load(Database& db, qint64 recordingId)
{
    beginResetModel();
    m_recording = recordingId;
    m_rows = TranscriptRepository(db).loadSegments(recordingId);
    m_hits.clear();
    m_currentHit = -1;
    endResetModel();
    rebuildHits();
}

void TranscriptModel::setSegmentText(int row, const QString& effective, bool edited)
{
    if (row < 0 || row >= m_rows.size()) return;
    m_rows[row].editedText = edited ? (effective.isNull() ? QStringLiteral("") : effective) : QString();   // "" = sentence deleted
    if (!edited) m_rows[row].text = effective;       // reverted: the effective text is the recognized one again
    rebuildHits();
    emit dataChanged(index(row), index(row));
}

void TranscriptModel::setSearchTerms(const QStringList& terms)
{
    m_regexes.clear();
    for (const QString& t : terms) {
        const QString n = ITokenizer::normalize(t).trimmed();
        if (n.isEmpty()) continue;
        m_regexes << QRegularExpression(QRegularExpression::escape(n), QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    }
    rebuildHits();
    emit highlightsChanged();            // a repaint, not dataChanged: that would re-measure every row (5000 sentences = seconds)
}

void TranscriptModel::rebuildHits()
{
    m_hits.clear();
    m_currentHit = -1;
    if (m_regexes.isEmpty()) return;
    for (int row = 0; row < m_rows.size(); ++row) {
        const QString text = m_rows[row].effective();
        // NFKC folds 全角/半角, but may change the length; only then are offsets unreliable, so fall back to the raw text
        QString folded = ITokenizer::normalize(text);
        if (folded.size() != text.size()) folded = text;
        QList<QPair<int, int>> ranges;
        for (const QRegularExpression& re : m_regexes) {
            auto it = re.globalMatch(folded);
            while (it.hasNext()) { const auto m = it.next(); ranges.append({ int(m.capturedStart()), int(m.capturedEnd()) }); }
        }
        std::sort(ranges.begin(), ranges.end());
        for (int i = 0; i < ranges.size();) {              // merge overlaps (two terms matching the same place)
            int from = ranges[i].first, to = ranges[i].second;
            for (++i; i < ranges.size() && ranges[i].first < to; ++i) to = std::max(to, ranges[i].second);
            m_hits.append({ row, from, to - from });
        }
    }
}

void TranscriptModel::setCurrentHit(int i)
{
    const int old = m_currentHit;
    m_currentHit = (i >= 0 && i < m_hits.size()) ? i : -1;
    emit highlightsChanged();
}

QVector<int> TranscriptModel::highlightsOf(int row) const
{
    QVector<int> v;
    for (const Hit& h : m_hits)
        if (h.row == row) v << h.start << h.length;
    return v;
}

int TranscriptModel::currentHitStartIn(int row) const
{
    return (m_currentHit >= 0 && m_hits[m_currentHit].row == row) ? m_hits[m_currentHit].start : -1;
}

int TranscriptModel::rowForTime(qint64 ms) const
{
    if (m_rows.isEmpty()) return -1;
    int lo = 0, hi = int(m_rows.size()) - 1, best = 0;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (m_rows[mid].startMs <= ms) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    return best;
}

QString TranscriptModel::fullText(bool withTimes) const
{
    QStringList lines;
    for (const auto& r : m_rows) lines << (withTimes ? "[" + formatTime(r.startMs) + "] " : QString()) + r.effective();
    return lines.join('\n');
}

} // namespace ake
