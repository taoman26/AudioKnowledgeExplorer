#include <QAction>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSqlError>
#include <QSqlQuery>
#include <QtTest>

#include "core/Database.h"
#include "core/ITokenizer.h"
#include "core/KnowledgeBuilder.h"
#include "core/TranscriptRepository.h"
#include "ui/CardListView.h"
#include "ui/DetailPane.h"
#include "ui/FilterChipBar.h"
#include "ui/MainWindow.h"
#include "ui/RecordingListModel.h"
#include "ui/SidebarWidget.h"

using namespace ake;

// search -> narrow by tag -> narrow by period (design.md "検索から絞り込む流れ")
class TstNarrowing : public QObject {
    Q_OBJECT
    Database db;
    std::unique_ptr<ITokenizer> tok;
    std::unique_ptr<MainWindow> w;
    qint64 docker = 0, study = 0, hiring = 0, memo = 0;

    qint64 add(const QString& relPath, const QStringList& segments, qint64 recordedAt)
    {
        QSqlQuery q(db.handle());
        q.prepare("INSERT INTO recordings(folder_id,rel_path,abs_path,status,recorded_at,created_at,updated_at) VALUES(1,?,?,'done',?,0,0)");
        q.addBindValue(relPath); q.addBindValue("/f/" + relPath); q.addBindValue(recordedAt);
        if (!q.exec()) qFatal("%s", qPrintable(q.lastError().text()));
        const qint64 id = q.lastInsertId().toLongLong();
        TranscriptResult r; r.language = "ja"; r.model = "t";
        qint64 t = 0;
        for (const QString& s : segments) { r.segments.append({ t, t + 3000, s }); t += 3000; }
        if (!TranscriptRepository(db).save(id, r)) qFatal("save");
        QString err;
        if (!KnowledgeBuilder(db, *tok).build(id, nullptr, &err)) qFatal("build %s", qPrintable(err));
        return id;
    }
    void start()
    {
        w = std::make_unique<MainWindow>();
        w->setTokenizer(tok.get());
        w->setDatabase(&db);
        w->show();
        QVERIFY(QTest::qWaitForWindowActive(w.get()));
    }
    void search(const QString& text) { w->searchBox()->setText(text); w->runSearchNow(); }
    const SidebarData& side() { return w->sidebarWidget()->data(); }
    QStringList tagNames() { QStringList l; for (const auto& t : side().tags) l << t.name; return l; }
    int rows() { return w->recordingModel()->rowCount(); }
    QStringList chipTexts() { QStringList l; for (const auto& c : w->chipBar()->chips()) l << c.text; return l; }
    void clickTag(qint64 id)
    {
        auto* sb = w->sidebarWidget();
        QTreeWidgetItem* it = sb->findItem(SidebarWidget::Tag, id);
        QVERIFY2(it, "tag not offered in the sidebar");
        emit sb->itemClicked(it, 0);
    }
    // a tag of the whole library that the current search result does not have (a dead end if it were offered)
    qint64 tagOutsideTheResult()
    {
        const SidebarData plain = SidebarData::load(db, QDateTime::currentDateTime());
        for (const auto& t : plain.tags) {
            bool offered = false;
            for (const auto& x : side().tags) offered |= x.id == t.id;
            if (!offered) return t.id;
        }
        return 0;
    }

private slots:
    void init()
    {
        tok = createTokenizer();
        QVERIFY(db.open(":memory:"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("INSERT INTO folders(id,path) VALUES(1,'/f')"));
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        docker = add("meetings/docker.m4a", { "今日はDocker ComposeからPodmanへの移行方針について議論します。",
                                               "Podmanはrootlessで動くので、セキュリティ面のメリットが大きいです。", "共有フォルダの権限は注意が必要です。" }, now - 3600);
        study = add("study/podman_study.mp3", { "Podmanの勉強会を始めます。", "rootlessコンテナの仕組みを説明します。",
                                                 "systemdとQuadletで自動起動できます。", "最後にPodmanの質問を受け付けます。" }, now - 86400LL * 400);   // last year
        hiring = add("interviews/hiring.flac", { "C++とSQLiteの経験について伺います。", "設計判断の理由を説明してください。" }, now - 7200);
        memo = add("memos/shopping.wav", { "牛乳とトマトを買って帰ります。" }, now - 86400LL * 40);
    }
    void cleanup() { w.reset(); }

    void withoutASearchTheSidebarCountsTheWholeLibrary()
    {
        start();
        QCOMPARE(side().all, 4);
        const int allTags = side().tags.size();
        QVERIFY(allTags > 3);
        clickTag(side().tags.first().id);                       // a tag filter alone does not turn the counts into facets
        QTest::qWait(30);
        QCOMPARE(side().all, 4);
        QCOMPARE(side().tags.size(), allTags);
    }
    void aSearchMakesTheSidebarCountInsideTheResult()
    {
        start();
        const QStringList before = tagNames();
        search("podman");
        QCOMPARE(rows(), 2);                                       // docker + study
        QCOMPARE(side().all, 2);
        QVERIFY(tagNames().size() < before.size());
        for (const auto& t : side().tags) QVERIFY2(t.count > 0 && t.count <= 2, qPrintable(t.name));   // no dead ends
        QCOMPARE(side().years.size(), 2);                          // this year and last year, both inside the result
        // leaving the search brings the whole library back
        w->searchBox()->clear();
        QTest::keyClick(w->searchBox(), Qt::Key_Escape);
        QTest::keyClick(w->searchBox(), Qt::Key_Escape);
        QTRY_COMPARE(side().all, 4);
        QCOMPARE(tagNames(), before);
    }
    void tagsOfTheResultNarrowItFurtherAndTheNextTagsFollow()
    {
        start();
        search("podman");
        // pick a tag that only one of the two recordings has: the result shrinks to 1 and the sidebar follows
        qint64 pick = 0;
        for (const auto& t : side().tags) if (t.count == 1) { pick = t.id; break; }
        QVERIFY(pick);
        clickTag(pick);
        QCOMPARE(rows(), 1);
        QTRY_COMPARE(side().all, 1);
        for (const auto& t : side().tags) QVERIFY2(t.count == 1, qPrintable(t.name));   // inside the one left
        bool selectedStillListed = false;
        for (const auto& t : side().tags) selectedStillListed |= t.id == pick;
        QVERIFY(selectedStillListed);
    }
    void chipsShowEveryConditionAndEachCanBeTakenOff()
    {
        start();
        QVERIFY(!w->chipBar()->isVisible());                      // nothing narrows: no bar
        search("podman");
        QCOMPARE(chipTexts(), QStringList({ "「podman」" }));
        QVERIFY(w->chipBar()->isVisible());
        QVERIFY(!w->chipBar()->clearAllButton()->isVisible());     // one condition: no "clear all"
        qint64 pick = 0; QString pickName;
        for (const auto& t : side().tags) if (t.count == 1) { pick = t.id; pickName = t.name; break; }
        clickTag(pick);
        QCOMPARE(chipTexts(), QStringList({ "「podman」", "#" + pickName }));
        QVERIFY(w->chipBar()->clearAllButton()->isVisible());
        QCOMPARE(w->chipBar()->chipButtons().size(), 2);
        // the ✕ of the tag chip takes only the tag off
        emit w->chipBar()->removeRequested(FilterChipBar::Tag, pick);
        QCOMPARE(chipTexts(), QStringList({ "「podman」" }));
        QCOMPARE(rows(), 2);
        QVERIFY(w->recordingModel()->isSearching());
        // the ✕ of the search chip clears the search box and keeps the filters
        clickTag(pick);
        emit w->chipBar()->removeRequested(FilterChipBar::Search, 0);
        QVERIFY(w->searchBox()->text().isEmpty());
        QVERIFY(!w->recordingModel()->isSearching());
        QCOMPARE(chipTexts(), QStringList({ "#" + pickName }));
        // "clear all" resets everything
        search("podman");
        w->chipBar()->clearAllButton()->click();
        QVERIFY(w->searchBox()->text().isEmpty());
        QVERIFY(w->sidebarWidget()->filter().isDefault());
        QCOMPARE(rows(), 4);
        QVERIFY(!w->chipBar()->isVisible());
    }
    void periodAndFolderConditionsGetChipsToo()
    {
        start();
        search("podman");
        RecordingFilter f = w->sidebarWidget()->filter();
        f.period = RecordingFilter::Period::ThisMonth;
        w->sidebarWidget()->setFilter(f);
        QTRY_COMPARE(chipTexts(), QStringList({ "「podman」", "今月" }));
        QCOMPARE(rows(), 1);                                        // only the docker recording is from this month
        QTRY_COMPARE(side().thisMonth, 1);
        f.scope = RecordingFilter::Scope::Favorites;
        w->sidebarWidget()->setFilter(f);
        QTRY_VERIFY(chipTexts().contains("お気に入り"));
        emit w->chipBar()->removeRequested(FilterChipBar::Scope, 0);
        emit w->chipBar()->removeRequested(FilterChipBar::Period, 0);
        QCOMPARE(chipTexts(), QStringList({ "「podman」" }));
        QCOMPARE(rows(), 2);
    }
    void whenTheFiltersEmptyTheResultTheMessageNamesThemAndOffersToTakeThemOff()
    {
        start();
        search("podman");
        const qint64 outside = tagOutsideTheResult();
        QVERIFY(outside);
        RecordingFilter f = w->sidebarWidget()->filter();
        f.tagIds << outside;                                        // not offered by the sidebar, but possible (shortcut, old selection)
        w->sidebarWidget()->setFilter(f);
        QCOMPARE(rows(), 0);
        QCOMPARE(w->emptyKind(), MainWindow::EmptyKind::NoMatches);
        QVERIFY2(w->emptyTitle()->text().contains("条件に一致"), qPrintable(w->emptyTitle()->text()));
        QTRY_VERIFY2(w->emptyText()->text().contains("「podman」") && w->emptyText()->text().contains("#"), qPrintable(w->emptyText()->text()));
        QVERIFY(w->emptyClearButton()->isVisible());
        QVERIFY(!w->emptyAddButton()->isVisible());
        QTest::mouseClick(w->emptyClearButton(), Qt::LeftButton);
        QVERIFY(w->sidebarWidget()->filter().isDefault());
        QCOMPARE(w->searchBox()->text(), QString("podman"));       // the search words stay
        QCOMPARE(rows(), 2);
        QCOMPARE(w->emptyKind(), MainWindow::EmptyKind::None);
    }
    void aSearchThatFindsNothingBlamesTheKeywordNotTheFilters()
    {
        start();
        search("zzzqqqxxx");
        QCOMPARE(w->emptyKind(), MainWindow::EmptyKind::NoMatches);
        QVERIFY(w->emptyTitle()->text().contains("該当"));
        QVERIFY(!w->emptyClearButton()->isVisible());              // there is nothing to take off
        QVERIFY(side().tags.isEmpty());
    }
    void recordingChangesWhileSearchingKeepTheFacets()
    {
        start();
        search("podman");
        const int n = side().tags.size();
        w->recordingModel()->refreshRecording(docker);
        emit w->detailPaneWidget()->recordingEdited(docker);        // e.g. a tag edit: the sidebar is recomputed inside the result
        QCOMPARE(side().all, 2);
        QCOMPARE(side().tags.size(), n);
    }
};

QTEST_MAIN(TstNarrowing)
#include "tst_narrowing_ui.moc"
