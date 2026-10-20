#pragma once
#include <QAbstractListModel>
#include <QDateTime>
#include <QHash>
#include <QStringList>
#include <QVector>

#include "core/RecordingFilter.h"
#include "core/SearchService.h"

namespace ake {

class Database;

enum class CardState { Done, Running, Error, Pending };

// Read-only list of recordings joined with their current title/summary/keywords (design.md section 6).
class RecordingListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        TitleRole,
        SummaryRole,
        KeywordsRole,
        RecordedAtRole, // QDateTime
        DurationMsRole,
        StateRole,      // CardState
        StatusRole,     // raw db status string
        ProgressRole,   // 0..1, or -1 if unknown
        FavoriteRole,
        ErrorRole,
        StageRole,      // live stage of a running job: decode | load | transcribe (empty otherwise)
        SnippetRole,    // search: text around the match (empty when not searching)
        HighlightsRole, // search: QVariantList [start, length, ...] into the snippet
        HitCountRole,   // search: transcript segments containing a term
        PlayingRole,    // this recording is being played right now
        SimilarRole,    // search: found by sound (a reading close to the typed word), not by spelling
    };
    enum class SortOrder { Newest, Oldest, Longest };

    struct Counts { int total = 0, done = 0, running = 0, pending = 0, error = 0; };

    explicit RecordingListModel(Database& db, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    void reload();
    void setSortOrder(SortOrder o);
    SortOrder sortOrder() const { return m_sort; }
    // Search mode: only the hits (still subject to the sidebar filter), best first, each with its snippet.
    void setSearchResult(const SearchResult& r);
    void clearSearch();
    bool isSearching() const { return m_searching; }
    bool isSimilarSearch() const { return m_similar; }          // nothing matched by spelling: the rows are candidates by sound
    QList<qint64> searchIds() const { return m_rank.keys(); }   // the recordings the search found (sidebar facets)
    void setPlaying(qint64 recordingId, bool playing);   // the card of the playing recording shows a pause button
    int searchSegmentHits() const; // sum of segment hits over the rows currently shown
    void setFilter(const RecordingFilter& f);
    const RecordingFilter& filter() const { return m_filter; }
    Counts counts() const;
    int rowForId(qint64 id) const;

    // Re-reads one recording without resetting the model (keeps selection and scroll position). Rows that no longer
    // match the filter are removed; a row that newly matches triggers a full reload.
    void refreshRecording(qint64 id);
    // Live progress of a running job, kept in memory only (no DB round-trip per tick).
    void setLiveProgress(qint64 id, const QString& stage, double overall);

private:
    struct Item {
        qint64 id = 0;
        QString title, summary, status, error;
        QStringList keywords;
        QDateTime recordedAt;
        qint64 durationMs = 0;
        double progress = -1;
        bool favorite = false;
    };
    struct Live { QString stage; double progress = 0; };
    QVector<Item> loadItems(const QString& extraWhere = QString(), const QVariantList& extraBinds = {}) const;

    Database& m_db;
    QHash<qint64, Live> m_live;
    QVector<Item> m_items;
    SortOrder m_sort = SortOrder::Newest;
    qint64 m_playingId = 0;
    bool m_searching = false;
    bool m_similar = false;
    QHash<qint64, SearchHit> m_hits;
    QHash<qint64, int> m_rank;
    RecordingFilter m_filter;
};

} // namespace ake
