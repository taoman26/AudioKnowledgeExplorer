#include "core/RecordingFilter.h"

#include <QSqlQuery>
#include <QStringList>

#include "core/Database.h"

namespace ake {

namespace {
const char* const kDate = "COALESCE(r.recorded_at, r.created_at)";

qint64 startOfWeek(const QDateTime& now)
{
    const QDate d = now.date();
    return QDateTime(d.addDays(-(d.dayOfWeek() - 1)), QTime(0, 0), now.timeSpec()).toSecsSinceEpoch();
}
qint64 startOfMonth(const QDateTime& now)
{
    return QDateTime(QDate(now.date().year(), now.date().month(), 1), QTime(0, 0), now.timeSpec()).toSecsSinceEpoch();
}
qint64 startOfYear(int y) { return QDateTime(QDate(y, 1, 1), QTime(0, 0)).toSecsSinceEpoch(); }

int scalarInt(QSqlQuery& q, const QString& sql, const QVariantList& binds = {})
{
    q.prepare(sql);
    for (const QVariant& b : binds) q.addBindValue(b);
    return (q.exec() && q.next()) ? q.value(0).toInt() : 0;
}
}

QString RecordingFilter::whereClause(const QDateTime& now, QVariantList* binds) const
{
    QStringList parts;
    switch (scope) {
    case Scope::All: break;
    case Scope::Favorites: parts << "r.favorite=1"; break;
    case Scope::Recent:
        parts << "r.created_at >= ?";
        binds->append(now.addDays(-kRecentDays).toSecsSinceEpoch());
        break;
    case Scope::Unanalyzed: parts << "r.status NOT IN ('done','error')"; break;
    case Scope::Error: parts << "r.status='error'"; break;
    case Scope::Folder:
        parts << "r.folder_id=?";
        binds->append(folderId);
        break;
    }
    switch (period) {
    case Period::None: break;
    case Period::ThisWeek:
        parts << QStringLiteral("%1 >= ?").arg(kDate);
        binds->append(startOfWeek(now));
        break;
    case Period::ThisMonth:
        parts << QStringLiteral("%1 >= ?").arg(kDate);
        binds->append(startOfMonth(now));
        break;
    case Period::Year:
        parts << QStringLiteral("%1 >= ? AND %1 < ?").arg(kDate);
        binds->append(startOfYear(year));
        binds->append(startOfYear(year + 1));
        break;
    }
    for (qint64 tag : tagIds) {
        parts << "EXISTS (SELECT 1 FROM recording_keywords rk WHERE rk.recording_id=r.id AND rk.keyword_id=?)";
        binds->append(tag);
    }
    return parts.isEmpty() ? QStringLiteral("1") : parts.join(" AND ");
}

SidebarData SidebarData::load(Database& db, const QDateTime& now, const FacetScope* facet)
{
    SidebarData d;
    QSqlQuery q(db.handle());
    // ids are numbers from our own search: safe to inline
    QString idClause;
    if (facet) {
        QStringList idl;
        for (qint64 id : facet->ids) idl << QString::number(id);
        idClause = idl.isEmpty() ? QStringLiteral(" AND 0") : " AND r.id IN (" + idl.join(',') + ")";
    }
    const RecordingFilter cur = facet ? facet->filter : RecordingFilter();
    auto count = [&](const RecordingFilter& f) {
        QVariantList b;
        return scalarInt(q, "SELECT count(*) FROM recordings r WHERE " + f.whereClause(now, &b) + idClause, b);
    };
    using Scope = RecordingFilter::Scope;
    // library items and folders: the result with the period and tags applied, but not their own (exclusive) selection
    RecordingFilter base;
    base.period = cur.period; base.year = cur.year; base.tagIds = cur.tagIds;
    RecordingFilter f = base;
    f.scope = Scope::All;        d.all = count(f);
    f.scope = Scope::Favorites;  d.favorites = count(f);
    f.scope = Scope::Recent;     d.recent = count(f);
    f.scope = Scope::Unanalyzed; d.unanalyzed = count(f);
    f.scope = Scope::Error;      d.error = count(f);
    // periods: the result with scope and tags applied, but not the period itself
    RecordingFilter inPeriod;
    inPeriod.scope = cur.scope; inPeriod.folderId = cur.folderId; inPeriod.tagIds = cur.tagIds;
    f = inPeriod; f.period = RecordingFilter::Period::ThisWeek;  d.thisWeek = count(f);
    f.period = RecordingFilter::Period::ThisMonth; d.thisMonth = count(f);

    if (!facet) {
        if (q.exec("SELECT f.id, f.path, (SELECT count(*) FROM recordings r WHERE r.folder_id=f.id) FROM folders f ORDER BY f.path"))
            while (q.next()) d.folders.append({ q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toInt() });
    } else {
        QList<SidebarData::Folder> all;
        if (q.exec("SELECT id, path FROM folders ORDER BY path"))
            while (q.next()) all.append({ q.value(0).toLongLong(), q.value(1).toString(), 0 });
        for (auto& fo : all) {
            RecordingFilter ff = base;
            ff.scope = Scope::Folder; ff.folderId = fo.id;
            fo.count = count(ff);
            if (fo.count > 0 || (cur.scope == Scope::Folder && cur.folderId == fo.id)) d.folders.append(fo);
        }
    }

    // tags: in the facet case, the tags of the recordings left by everything selected (so every tag offered narrows
    // the result), most used first; selected tags are always listed
    QVariantList tb;
    const QString tagWhere = facet ? cur.whereClause(now, &tb) + idClause : QStringLiteral("1");
    const QString tagSql = QStringLiteral("SELECT k.id, k.name, COUNT(DISTINCT rk.recording_id) c FROM keywords k"
                                          " JOIN recording_keywords rk ON rk.keyword_id=k.id"
                                          " JOIN recordings r ON r.id=rk.recording_id WHERE %1 GROUP BY k.id"
                                          " ORDER BY c DESC, k.name COLLATE NOCASE").arg(tagWhere);
    q.prepare(tagSql);
    for (const QVariant& b : tb) q.addBindValue(b);
    if (q.exec())
        while (q.next()) d.tags.append({ q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toInt() });
    if (facet) {
        for (qint64 id : cur.tagIds) {
            bool listed = false;
            for (const auto& t : d.tags) listed |= t.id == id;
            if (listed) continue;
            QSqlQuery n(db.handle());
            n.prepare("SELECT name FROM keywords WHERE id=?");
            n.addBindValue(id);
            if (n.exec() && n.next()) d.tags.append({ id, n.value(0).toString(), 0 });
        }
    }

    // years: the result with scope and tags applied, whatever period is selected (so another year can be picked)
    QVariantList yb;
    const QString yearWhere = inPeriod.whereClause(now, &yb) + idClause;
    // strftime on local time so the year matches what startOfYear() filters on
    q.prepare(QStringLiteral("SELECT CAST(strftime('%Y', %1, 'unixepoch', 'localtime') AS INTEGER) y, count(*) FROM recordings r"
                             " WHERE %2 GROUP BY y ORDER BY y DESC").arg(kDate, yearWhere));
    for (const QVariant& b : yb) q.addBindValue(b);
    if (q.exec())
        while (q.next()) d.years.append({ q.value(0).toInt(), q.value(1).toInt() });
    if (facet && cur.period == RecordingFilter::Period::Year) {       // the selected year stays listed, even when empty
        bool listed = false;
        for (const auto& y : d.years) listed |= y.year == cur.year;
        if (!listed) d.years.prepend({ cur.year, 0 });
    }
    return d;
}

} // namespace ake
