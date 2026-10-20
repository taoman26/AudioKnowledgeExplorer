#include <QSqlError>
#include <QSqlQuery>
#include <QtTest>

#include "core/Database.h"
#include "core/DemoData.h"
#include "core/RecordingFilter.h"

using namespace ake;
using Scope = RecordingFilter::Scope;
using Period = RecordingFilter::Period;

class TstFilter : public QObject {
    Q_OBJECT
    Database db;

    QStringList run(const RecordingFilter& f, const QDateTime& now)
    {
        QVariantList b;
        QSqlQuery q(db.handle());
        q.prepare("SELECT r.rel_path FROM recordings r WHERE " + f.whereClause(now, &b) + " ORDER BY r.id");
        for (auto& v : b) q.addBindValue(v);
        QStringList out;
        if (q.exec()) while (q.next()) out << q.value(0).toString();
        else qWarning() << q.lastError().text();
        return out;
    }
    qint64 tagId(const QString& name)
    {
        QSqlQuery q(db.handle());
        q.prepare("SELECT id FROM keywords WHERE name=?");
        q.addBindValue(name);
        return (q.exec() && q.next()) ? q.value(0).toLongLong() : -1;
    }
    void exec(const QString& sql) { QSqlQuery q(db.handle()); QVERIFY2(q.exec(sql), qPrintable(q.lastError().text())); }
    const QDateTime sunday = QDateTime(QDate(2026, 9, 13), QTime(12, 0)); // week = Mon 2026-09-07 ..

    QList<qint64> idsWithTag(const QString& tag)
    {
        QList<qint64> ids;
        QSqlQuery q(db.handle());
        q.prepare("SELECT rk.recording_id FROM recording_keywords rk JOIN keywords k ON k.id=rk.keyword_id WHERE k.name=? ORDER BY 1");
        q.addBindValue(tag);
        if (q.exec()) while (q.next()) ids << q.value(0).toLongLong();
        return ids;
    }
    QList<qint64> allIds()
    {
        QList<qint64> ids;
        QSqlQuery q(db.handle());
        if (q.exec("SELECT id FROM recordings ORDER BY id")) while (q.next()) ids << q.value(0).toLongLong();
        return ids;
    }
    static QStringList names(const QList<SidebarData::Tag>& t) { QStringList l; for (const auto& x : t) l << x.name; return l; }
    static int countOf(const QList<SidebarData::Tag>& t, const QString& n) { for (const auto& x : t) if (x.name == n) return x.count; return -1; }

private slots:
    void init()
    {
        QVERIFY(db.open(":memory:"));
        QVERIFY(seedDemoData(db));
        QCOMPARE(sunday.date().dayOfWeek(), 7);
    }
    // ---- facets: the sidebar counts inside the search result
    void facetOverTheWholeLibraryEqualsThePlainCounts()
    {
        const SidebarData plain = SidebarData::load(db, sunday);
        const FacetScope all{ allIds(), RecordingFilter() };
        const SidebarData f = SidebarData::load(db, sunday, &all);
        QCOMPARE(f.all, plain.all); QCOMPARE(f.favorites, plain.favorites); QCOMPARE(f.error, plain.error);
        QCOMPARE(f.thisWeek, plain.thisWeek); QCOMPARE(f.thisMonth, plain.thisMonth);
        QCOMPARE(f.folders.size(), plain.folders.size());
        QCOMPARE(f.years.size(), plain.years.size());
        QCOMPARE(names(f.tags), names(plain.tags));
        for (const auto& t : plain.tags) QCOMPARE(countOf(f.tags, t.name), t.count);
    }
    void facetOffersOnlyTagsOfTheResultAndCountsInsideIt()
    {
        const QList<qint64> hits = idsWithTag("podman");
        QVERIFY(hits.size() >= 2);
        const FacetScope scope{ hits, RecordingFilter() };
        const SidebarData d = SidebarData::load(db, sunday, &scope);
        QCOMPARE(d.all, int(hits.size()));
        QCOMPARE(countOf(d.tags, "podman"), int(hits.size()));
        QCOMPARE(countOf(d.tags, "C++"), -1);                              // not carried by any recording of the result
        QVERIFY(d.tags.size() < SidebarData::load(db, sunday).tags.size());
        for (const auto& t : d.tags) QVERIFY2(t.count > 0, qPrintable(t.name));   // nothing offered leads to an empty list
        QVERIFY(d.favorites <= d.all);
    }
    void facetSecondLevelNarrowingAppliesTheSelectedTags()
    {
        RecordingFilter sel; sel.tagIds << tagId("podman");
        const FacetScope scope{ allIds(), sel };
        const SidebarData d = SidebarData::load(db, sunday, &scope);
        QCOMPARE(d.all, int(idsWithTag("podman").size()));                  // counts are those of the narrowed result
        QCOMPARE(countOf(d.tags, "podman"), d.all);                         // the selected tag itself: all of them
        QCOMPARE(countOf(d.tags, "C++"), -1);
    }
    void facetKeepsSelectedTagsListedEvenWhenTheResultHasNone()
    {
        RecordingFilter sel; sel.tagIds << tagId("C++");
        const FacetScope scope{ idsWithTag("podman"), sel };               // C++ and podman never meet in the demo data
        const SidebarData d = SidebarData::load(db, sunday, &scope);
        QCOMPARE(d.all, 0);
        QCOMPARE(countOf(d.tags, "C++"), 0);                                // still there, so the user can take it off
        QCOMPARE(d.tags.size(), 1);
    }
    void facetCountsEachGroupWithoutItsOwnSelection()
    {
        // period: the other periods stay countable while one is selected
        RecordingFilter sel; sel.period = Period::ThisWeek;
        const FacetScope scope{ allIds(), sel };
        const SidebarData d = SidebarData::load(db, sunday, &scope);
        const SidebarData all = SidebarData::load(db, sunday);
        QCOMPARE(d.thisMonth, all.thisMonth);                               // not narrowed by "this week"
        QCOMPARE(d.all, all.thisWeek);                                      // the library counts are
        // scope: with a folder selected, the library items still count the whole result
        exec("INSERT INTO folders(id,path) VALUES(2,'/other')");
        exec("INSERT INTO recordings(folder_id,rel_path,abs_path,created_at,updated_at) VALUES(2,'z.mp3','/other/z.mp3',0,0)");
        RecordingFilter fsel; fsel.scope = Scope::Folder; fsel.folderId = 2;
        const FacetScope s2{ allIds(), fsel };
        const SidebarData e = SidebarData::load(db, sunday, &s2);
        QCOMPARE(e.all, 10);
        QCOMPARE(e.folders.size(), 2);                                      // both folders still offered
    }
    void facetOfAnEmptyResultOffersNothing()
    {
        const FacetScope none{ {}, RecordingFilter() };
        const SidebarData d = SidebarData::load(db, sunday, &none);
        QCOMPARE(d.all, 0); QCOMPARE(d.thisWeek, 0);
        QVERIFY(d.tags.isEmpty());
        QVERIFY(d.folders.isEmpty());
        QVERIFY(d.years.isEmpty());
        RecordingFilter y; y.period = Period::Year; y.year = 2026;
        const FacetScope selYear{ {}, y };
        const SidebarData e = SidebarData::load(db, sunday, &selYear);
        QCOMPARE(e.years.size(), 1);                                        // a selected year stays, as 0
        QCOMPARE(e.years[0].count, 0);
    }
    void defaultFilterMatchesEverything()
    {
        RecordingFilter f;
        QVERIFY(f.isDefault());
        QCOMPARE(run(f, sunday).size(), 9);
    }
    void libraryScopes()
    {
        RecordingFilter f;
        f.scope = Scope::Favorites;  QCOMPARE(run(f, sunday), QStringList({ "meetings/docker.m4a" }));
        f.scope = Scope::Error;      QCOMPARE(run(f, sunday), QStringList({ "broken/broken_rec.flac" }));
        f.scope = Scope::Unanalyzed; QCOMPARE(run(f, sunday).size(), 2); // running + queued; not done, not error
    }
    void recentUsesCreatedAtWindow()
    {
        const QDateTime now = QDateTime::currentDateTime();
        RecordingFilter f; f.scope = Scope::Recent;
        QCOMPARE(run(f, now).size(), 9); // seeded just now
        exec("UPDATE recordings SET created_at=created_at-86400*30 WHERE rel_path LIKE 'memos/%'");
        QCOMPARE(run(f, now).size(), 7);
    }
    void folderScope()
    {
        exec("INSERT INTO folders(id,path) VALUES(2,'/other')");
        exec("INSERT INTO recordings(folder_id,rel_path,abs_path,created_at,updated_at) VALUES(2,'z.mp3','/other/z.mp3',0,0)");
        RecordingFilter f; f.scope = Scope::Folder; f.folderId = 2;
        QCOMPARE(run(f, sunday), QStringList({ "z.mp3" }));
        f.folderId = 1;
        QCOMPARE(run(f, sunday).size(), 9);
    }
    void periodsThisWeekMonthYear()
    {
        RecordingFilter f;
        f.period = Period::ThisWeek;   // Mon 09-07..: docker (Sat 09-12) only
        QCOMPARE(run(f, sunday), QStringList({ "meetings/docker.m4a" }));
        f.period = Period::ThisMonth;  // 2026-09
        QCOMPARE(run(f, sunday), QStringList({ "meetings/docker.m4a" }));
        const QDateTime monday = QDateTime(QDate(2026, 9, 14), QTime(0, 5)); // new week: docker is last week
        f.period = Period::ThisWeek;
        QCOMPARE(run(f, monday).size(), 0);
        f.period = Period::Year; f.year = 2026;
        QCOMPARE(run(f, sunday).size(), 9);
        f.year = 2025;
        QCOMPARE(run(f, sunday).size(), 0);
    }
    void tagsCombineWithAnd()
    {
        RecordingFilter f;
        f.tagIds = { tagId("podman") };
        QCOMPARE(run(f, sunday), QStringList({ "meetings/docker.m4a", "study/podman3.mp3" }));
        f.tagIds = { tagId("podman"), tagId("docker") };
        QCOMPARE(run(f, sunday), QStringList({ "meetings/docker.m4a" }));
        f.tagIds = { tagId("podman"), tagId("rust") };
        QCOMPARE(run(f, sunday).size(), 0);
    }
    void filtersCombineAcrossKinds()
    {
        RecordingFilter f;
        f.scope = Scope::Favorites; f.period = Period::ThisMonth; f.tagIds = { tagId("linux") };
        QCOMPARE(run(f, sunday), QStringList({ "meetings/docker.m4a" }));
        f.scope = Scope::Error;
        QCOMPARE(run(f, sunday).size(), 0);
    }
    void sidebarCountsMatchFilters()
    {
        const SidebarData d = SidebarData::load(db, sunday);
        QCOMPARE(d.all, 9);
        QCOMPARE(d.favorites, 1);
        QCOMPARE(d.unanalyzed, 2);
        QCOMPARE(d.error, 1);
        QCOMPARE(d.thisWeek, 1);
        QCOMPARE(d.thisMonth, 1);
        QCOMPARE(d.folders.size(), 1);
        QCOMPARE(d.folders[0].count, 9);
        QCOMPARE(d.years.size(), 1);
        QCOMPARE(d.years[0].year, 2026);
        QCOMPARE(d.years[0].count, 9);
        QVERIFY(d.tags.size() >= 10);
        QCOMPARE(d.tags[0].name, QString("podman")); // used twice: first
        QCOMPARE(d.tags[0].count, 2);
        for (int i = 1; i < d.tags.size(); ++i) QVERIFY(d.tags[i - 1].count >= d.tags[i].count);
    }
};

QTEST_MAIN(TstFilter)
#include "tst_filter.moc"
