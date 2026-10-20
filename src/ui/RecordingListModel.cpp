#include "ui/RecordingListModel.h"

#include <QFileInfo>
#include <QHash>
#include <algorithm>
#include <QSqlQuery>

#include "core/Database.h"

namespace ake {

RecordingListModel::RecordingListModel(Database& db, QObject* parent)
    : QAbstractListModel(parent), m_db(db)
{
    reload();
}

int RecordingListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_items.size();
}

static CardState stateOf(const QString& status)
{
    if (status == "done") return CardState::Done;
    if (status == "transcribing" || status == "analyzing") return CardState::Running;
    if (status == "error") return CardState::Error;
    return CardState::Pending;
}

QVariant RecordingListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_items.size())
        return {};
    const Item& it = m_items[index.row()];
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole: return it.title;
    case IdRole: return it.id;
    case SummaryRole: return it.summary;
    case KeywordsRole: return it.keywords;
    case RecordedAtRole: return it.recordedAt;
    case DurationMsRole: return it.durationMs;
    case StateRole: return QVariant::fromValue(stateOf(it.status));
    case StatusRole: return it.status;
    case ProgressRole: {
        auto live = m_live.constFind(it.id);
        return live != m_live.constEnd() ? live->progress : it.progress;
    }
    case SnippetRole: return m_hits.contains(it.id) ? m_hits[it.id].snippet : QString();
    case HighlightsRole: {
        QVariantList l;
        if (m_hits.contains(it.id)) for (int v : m_hits[it.id].highlights) l << v;
        return l;
    }
    case PlayingRole: return it.id == m_playingId;
    case HitCountRole: return m_hits.contains(it.id) ? m_hits[it.id].segmentHits : 0;
    case SimilarRole: return m_hits.contains(it.id) && m_hits[it.id].similar;
    case StageRole: {
        auto live = m_live.constFind(it.id);
        return live != m_live.constEnd() ? live->stage : QString();
    }
    case FavoriteRole: return it.favorite;
    case ErrorRole: return it.error;
    }
    return {};
}

void RecordingListModel::setSortOrder(SortOrder o)
{
    if (o == m_sort) return;
    m_sort = o;
    reload();
}

void RecordingListModel::setSearchResult(const SearchResult& r)
{
    m_searching = true;
    m_similar = r.similar;
    m_hits.clear();
    m_rank.clear();
    for (int i = 0; i < r.hits.size(); ++i) {
        m_hits.insert(r.hits[i].recordingId, r.hits[i]);
        m_rank.insert(r.hits[i].recordingId, i);
    }
    reload();
}

void RecordingListModel::clearSearch()
{
    if (!m_searching) return;
    m_searching = false;
    m_similar = false;
    m_hits.clear();
    m_rank.clear();
    reload();
}

void RecordingListModel::setPlaying(qint64 id, bool playing)
{
    const qint64 now = playing ? id : 0;
    if (now == m_playingId) return;
    const qint64 old = m_playingId;
    m_playingId = now;
    for (qint64 rid : { old, now }) {
        const int row = rowForId(rid);
        if (row >= 0) emit dataChanged(index(row), index(row), { PlayingRole });
    }
}

int RecordingListModel::searchSegmentHits() const
{
    int n = 0;
    for (const Item& it : m_items) n += m_hits.value(it.id).segmentHits;
    return n;
}

void RecordingListModel::setFilter(const RecordingFilter& f)
{
    if (f == m_filter) return;
    m_filter = f;
    reload();
}

QVector<RecordingListModel::Item> RecordingListModel::loadItems(const QString& extraWhere, const QVariantList& extraBinds) const
{
    const char* order = m_sort == SortOrder::Oldest ? "COALESCE(r.recorded_at, r.created_at) ASC, r.id ASC"
                      : m_sort == SortOrder::Longest ? "r.duration_ms DESC, r.id ASC"
                                                     : "COALESCE(r.recorded_at, r.created_at) DESC, r.id DESC";
    QVariantList binds;
    QString where = m_filter.whereClause(QDateTime::currentDateTime(), &binds);
    if (!extraWhere.isEmpty()) {
        where += " AND " + extraWhere;
        binds += extraBinds;
    }
    if (m_searching) {          // ids are numbers from our own search: safe to inline
        QStringList idl;
        for (auto it = m_rank.cbegin(); it != m_rank.cend(); ++it) idl << QString::number(it.key());
        where += idl.isEmpty() ? QStringLiteral(" AND 0") : " AND r.id IN (" + idl.join(',') + ")";
    }
    const QString sql = QStringLiteral(
        "SELECT r.id, r.rel_path, r.status, r.duration_ms, COALESCE(r.recorded_at, r.created_at), r.favorite, r.error_msg,"
        " (SELECT NULLIF(content,'') FROM analyses a WHERE a.recording_id=r.id AND a.kind='title' AND a.is_current=1 ORDER BY a.id DESC LIMIT 1),"
        " (SELECT content FROM analyses a WHERE a.recording_id=r.id AND a.kind='summary' AND a.is_current=1 ORDER BY a.id DESC LIMIT 1),"
        " (SELECT progress FROM jobs j WHERE j.recording_id=r.id AND j.state='running' ORDER BY j.id DESC LIMIT 1),"
        " EXISTS (SELECT 1 FROM segments s JOIN transcripts t ON t.id=s.transcript_id WHERE t.recording_id=r.id AND t.is_current=1)"
        " FROM recordings r WHERE %1 ORDER BY %2").arg(where, order);

    QVector<Item> items;
    QSqlQuery q(m_db.handle());
    q.prepare(sql);
    for (const QVariant& b : binds) q.addBindValue(b);
    if (q.exec()) {
        while (q.next()) {
            Item it;
            it.id = q.value(0).toLongLong();
            it.status = q.value(2).toString();
            it.durationMs = q.value(3).toLongLong();
            it.recordedAt = QDateTime::fromSecsSinceEpoch(q.value(4).toLongLong());
            it.favorite = q.value(5).toBool();
            it.error = q.value(6).toString();
            it.title = q.value(7).isNull() ? QFileInfo(q.value(1).toString()).fileName() : q.value(7).toString();
            it.summary = q.value(8).toString();
            it.progress = q.value(9).isNull() ? -1 : q.value(9).toDouble();
            if (it.status == "done" && !q.value(10).toBool() && it.summary.isEmpty())   // analysed, but nothing was said (noise, silence)
                it.summary = QObject::tr("（音声が検出されませんでした）");
            items.append(it);
        }
    }
    if (m_searching)            // relevance order replaces the normal sort
        std::stable_sort(items.begin(), items.end(), [&](const Item& x, const Item& y) { return m_rank.value(x.id) < m_rank.value(y.id); });
    QHash<qint64, int> rowById;
    for (int i = 0; i < items.size(); ++i) rowById.insert(items[i].id, i);
    // keywords for the loaded rows (ordered by score)
    if (q.exec("SELECT rk.recording_id, k.name FROM recording_keywords rk JOIN keywords k ON k.id=rk.keyword_id"
               " ORDER BY rk.recording_id, rk.score DESC, k.name COLLATE NOCASE")) {
        while (q.next()) {
            auto it = rowById.constFind(q.value(0).toLongLong());
            if (it != rowById.constEnd()) items[*it].keywords << q.value(1).toString();
        }
    }
    return items;
}

void RecordingListModel::reload()
{
    QVector<Item> items = loadItems();
    beginResetModel();
    m_items = std::move(items);
    // forget live progress of recordings that are no longer running
    for (auto it = m_live.begin(); it != m_live.end();) {
        const int row = rowForId(it.key());
        if (row >= 0 && stateOf(m_items[row].status) == CardState::Running) ++it;
        else it = m_live.erase(it);
    }
    endResetModel();
}

void RecordingListModel::refreshRecording(qint64 id)
{
    const int row = rowForId(id);
    const QVector<Item> now = loadItems("r.id=?", { id });
    if (row < 0) {
        if (!now.isEmpty()) reload(); // newly matches the filter: ordering matters, so rebuild
        return;
    }
    if (now.isEmpty()) {              // no longer matches (or was deleted)
        beginRemoveRows({}, row, row);
        m_items.removeAt(row);
        m_live.remove(id);
        endRemoveRows();
        return;
    }
    m_items[row] = now.first();
    if (stateOf(m_items[row].status) != CardState::Running) m_live.remove(id);
    emit dataChanged(index(row), index(row));
}

void RecordingListModel::setLiveProgress(qint64 id, const QString& stage, double overall)
{
    const int row = rowForId(id);
    if (row < 0) return;
    m_live[id] = { stage, overall };
    emit dataChanged(index(row), index(row), { ProgressRole, StageRole });
}

RecordingListModel::Counts RecordingListModel::counts() const
{
    Counts c;
    c.total = m_items.size();
    for (const Item& it : m_items) {
        switch (stateOf(it.status)) {
        case CardState::Done: c.done++; break;
        case CardState::Running: c.running++; break;
        case CardState::Error: c.error++; break;
        case CardState::Pending: c.pending++; break;
        }
    }
    return c;
}

int RecordingListModel::rowForId(qint64 id) const
{
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items[i].id == id) return i;
    return -1;
}

} // namespace ake
