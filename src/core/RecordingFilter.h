#pragma once
#include <QDateTime>
#include <QList>
#include <QString>
#include <QVariantList>

namespace ake {

class Database;

// What the sidebar selects (design.md section 5). Scope, period and tags combine with AND.
struct RecordingFilter {
    enum class Scope { All, Favorites, Recent, Unanalyzed, Error, Folder };
    enum class Period { None, ThisWeek, ThisMonth, Year };

    Scope scope = Scope::All;
    qint64 folderId = 0;      // Scope::Folder
    Period period = Period::None;
    int year = 0;             // Period::Year
    QList<qint64> tagIds;     // every tag must match (AND)

    bool isDefault() const { return scope == Scope::All && period == Period::None && tagIds.isEmpty(); }
    bool operator==(const RecordingFilter& o) const
    {
        return scope == o.scope && folderId == o.folderId && period == o.period && year == o.year && tagIds == o.tagIds;
    }

    static constexpr int kRecentDays = 14;

    // SQL fragment over alias `r` (recordings), "1" when unfiltered. `now` is injectable for tests.
    // Week starts on Monday. Dates compare against recorded_at, falling back to created_at.
    QString whereClause(const QDateTime& now, QVariantList* binds) const;
};

// Counts "inside the search result": the recordings the search found, narrowed by what is selected in the sidebar.
struct FacetScope {
    QList<qint64> ids;       // recordings the search found
    RecordingFilter filter;  // what is selected now
};

struct SidebarData {
    struct Folder { qint64 id; QString path; int count; };
    struct Tag { qint64 id; QString name; int count; };
    struct Year { int year; int count; };

    int all = 0, favorites = 0, recent = 0, unanalyzed = 0, error = 0;
    int thisWeek = 0, thisMonth = 0;
    QList<Folder> folders;
    QList<Tag> tags;   // all tags, most used first
    QList<Year> years; // newest first

    // Without `facet`: counts over the whole library. With it (a search is active): counts over the search result, so
    // the sidebar only offers what narrows it further (design.md "検索から絞り込む流れ"): tags with no recording in
    // the result are left out (selected ones stay, to be removable); each group is counted without its own selection.
    static SidebarData load(Database& db, const QDateTime& now, const FacetScope* facet = nullptr);
};

} // namespace ake
