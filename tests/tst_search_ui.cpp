#include <QAction>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QtTest>

#include "core/Database.h"
#include "core/ITokenizer.h"
#include "core/KnowledgeBuilder.h"
#include "core/SearchService.h"
#include "core/TranscriptRepository.h"
#include "ui/CardListView.h"
#include "ui/MainWindow.h"
#include "ui/RecordingListModel.h"
#include "ui/SearchSuggestPopup.h"
#include "ui/SidebarWidget.h"

using namespace ake;

class TstSearchUi : public QObject {
    Q_OBJECT
    Database db;
    std::unique_ptr<ITokenizer> tok;
    std::unique_ptr<MainWindow> w;
    qint64 docker = 0, study = 0, hiring = 0, memo = 0;
    int m_n = 0;

    qint64 add(const QString& relPath, const QStringList& segments)
    {
        QSqlQuery q(db.handle());
        q.prepare("INSERT INTO recordings(folder_id,rel_path,abs_path,status,recorded_at,created_at,updated_at) VALUES(1,?,?,'done',?,0,0)");
        q.addBindValue(relPath); q.addBindValue("/f/" + relPath); q.addBindValue(1000000 + m_n++);
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
    RecordingListModel* model() { return w->recordingModel(); }
    QLineEdit* box() { return w->searchBox(); }
    QString headerTitle() { return w->findChild<QLabel*>("cardTitle")->text(); }
    QString headerCount() { return w->findChild<QLabel*>("cardCount")->text(); }
    QList<qint64> shown()
    {
        QList<qint64> l;
        for (int r = 0; r < model()->rowCount(); ++r) l << model()->index(r, 0).data(RecordingListModel::IdRole).toLongLong();
        return l;
    }
    void type(const QString& text)
    {
        box()->setFocus();
        QTest::keyClicks(box(), text);
    }
    void startWindow(bool withTokenizer = true)
    {
        w = std::make_unique<MainWindow>();
        if (withTokenizer) w->setTokenizer(tok.get());
        w->setDatabase(&db);
        w->show();
        QVERIFY(QTest::qWaitForWindowActive(w.get()));
    }

private slots:
    void init()
    {
        tok = createTokenizer();
        QVERIFY(db.open(":memory:"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("INSERT INTO folders(id,path) VALUES(1,'/f')"));
        m_n = 0;
        docker = add("meetings/docker.m4a", { "今日はDocker ComposeからPodmanへの移行方針について議論します。",
                                               "Podmanはrootlessで動くので、セキュリティ面のメリットが大きいです。",
                                               "共有フォルダの権限は注意が必要です。", "移行は段階的に進めます。" });
        study = add("study/podman_study.mp3", { "Podmanの勉強会を始めます。", "rootlessコンテナの仕組みを説明します。", "systemdとQuadletで自動起動できます。" });
        hiring = add("interviews/hiring.flac", { "C++とSQLiteの経験について伺います。", "設計判断の理由を説明してください。" });
        memo = add("memos/shopping.wav", { "牛乳とトマトを買って帰ります。" });
    }
    void cleanup() { w.reset(); }

    void whenNothingMatchesBySpellingTheRowsAreCandidatesBySound()
    {
        if (!tok->hasReadings()) QSKIP("needs a tokenizer with readings (MeCab)");
        const qint64 near = add("sound/near.mp3", { "前の話です。", "配当の段階に進みます。", "次の話です。" });   // はいとう ~ かいとう
        startWindow();
        box()->setText("回答");
        w->runSearchNow();
        QVERIFY(model()->isSearching());
        QVERIFY(model()->isSimilarSearch());
        QCOMPARE(shown(), QList<qint64>({ near }));
        const QModelIndex i = model()->index(0, 0);
        QVERIFY(i.data(RecordingListModel::SimilarRole).toBool());
        QVERIFY(i.data(RecordingListModel::SnippetRole).toString().contains("配当"));
        QVERIFY(i.data(RecordingListModel::HighlightsRole).toList().isEmpty());        // no spelled match to highlight
        QVERIFY2(headerCount().contains("に一致する録音はありません") && headerCount().contains("読みが近い候補 1件"), qPrintable(headerCount()));
        QCOMPARE(w->sidebarWidget()->data().all, 1);                                   // the sidebar counts inside these candidates
        // typing something that matches by spelling leaves the by-sound mode
        box()->setText("podman");
        w->runSearchNow();
        QVERIFY(!model()->isSimilarSearch());
        QVERIFY(!model()->index(0, 0).data(RecordingListModel::SimilarRole).toBool());
        QVERIFY(!headerCount().contains("読みが近い"));
        // clearing the search clears the mode as well
        box()->setText("回答");
        w->runSearchNow();
        QVERIFY(model()->isSimilarSearch());
        w->searchBox()->clear();
        QTest::keyClick(w->searchBox(), Qt::Key_Escape);
        QVERIFY(!model()->isSimilarSearch());
    }
    void typingSearchesAfterTheDebounceAndShowsSnippets()
    {
        startWindow();
        QSignalSpy reset(model(), &QAbstractItemModel::modelReset);
        type("podman");
        QVERIFY2(!model()->isSearching(), "must wait for typing to pause");      // still inside the 200 ms window
        QCOMPARE(model()->rowCount(), 4);
        QTRY_VERIFY_WITH_TIMEOUT(model()->isSearching(), 2000);
        QCOMPARE(reset.count(), 1);                                              // six keystrokes, one search
        QCOMPARE(model()->rowCount(), 2);
        QCOMPARE(headerTitle(), QString("検索結果"));
        QVERIFY2(headerCount().contains("「podman」") && headerCount().contains("2件") && headerCount().contains("発言"), qPrintable(headerCount()));
        for (int r = 0; r < 2; ++r) {
            const QModelIndex i = model()->index(r, 0);
            QVERIFY(i.data(RecordingListModel::SnippetRole).toString().toLower().contains("podman"));
            QVERIFY(!i.data(RecordingListModel::HighlightsRole).toList().isEmpty());
            QVERIFY(i.data(RecordingListModel::HitCountRole).toInt() >= 1);
        }
        QVERIFY(shown().contains(docker) && shown().contains(study));
    }
    void resultsAreOrderedByRelevanceNotByDate()
    {
        // whatever the service ranks first, make it the OLDEST recording: relevance order and date order now disagree
        SearchService svc(db, *tok);
        const SearchResult r = svc.search("podman");
        QVERIFY(svc.lastError().isEmpty());
        QCOMPARE(r.hits.size(), 2);
        const qint64 best = r.hits[0].recordingId, other = r.hits[1].recordingId;
        QSqlQuery q(db.handle());
        QVERIFY(q.exec(QString("UPDATE recordings SET recorded_at=1 WHERE id=%1").arg(best)));
        QVERIFY(q.exec(QString("UPDATE recordings SET recorded_at=2000000 WHERE id=%1").arg(other)));
        startWindow();
        const QList<qint64> normal = shown();
        QVERIFY(normal.indexOf(other) < normal.indexOf(best));          // normal order: newest first
        box()->setText("podman");
        w->runSearchNow();
        QCOMPARE(shown(), QList<qint64>({ best, other }));              // search order: most relevant first
        box()->clear();
        QCOMPARE(shown(), normal);                                      // and back
    }
    void enterRunsAtOnceAndClosesTheSuggestions()
    {
        startWindow();
        box()->setFocus();
        box()->setText("rootless");
        QTest::keyClick(box(), Qt::Key_Return);
        QVERIFY(model()->isSearching());
        QCOMPARE(model()->rowCount(), 2);
        QVERIFY(!w->searchPopup()->isVisible());
    }
    void suggestionsAppearWithSectionsAndHighlightedSnippet()
    {
        startWindow();
        box()->setFocus();
        box()->setText("podm");
        w->runSearchNow();
        QVERIFY(w->searchPopup()->isVisible());
        QVERIFY(w->searchPopup()->itemCountOfKind(SearchSuggestPopup::TagItem) >= 1);
        QVERIFY(w->searchPopup()->itemCountOfKind(SearchSuggestPopup::RecordingItem) >= 1);   // podman_study.mp3 by file name
        QVERIFY(w->searchPopup()->itemCountOfKind(SearchSuggestPopup::SegmentItem) >= 1);
        QVERIFY(w->searchPopup()->geometry().top() >= box()->mapTo(w.get(), QPoint(0, 0)).y() + box()->height());
        QCOMPARE(w->searchPopup()->width(), box()->width());
        // grabbing must not crash and must contain pixels (delegate paints highlights)
        QVERIFY(!w->searchPopup()->grab().isNull());
    }
    void arrowKeysAndEnterChooseASuggestion()
    {
        startWindow();
        box()->setFocus();
        box()->setText("podm");
        w->runSearchNow();
        QVERIFY(!w->searchPopup()->hasSelection());
        QTest::keyClick(box(), Qt::Key_Down);                                    // first selectable row: the tag
        QVERIFY(w->searchPopup()->hasSelection());
        QCOMPARE(w->searchPopup()->currentItem()->data(SearchSuggestPopup::KindRole).toInt(), int(SearchSuggestPopup::TagItem));
        QTest::keyClick(box(), Qt::Key_Down);
        QTest::keyClick(box(), Qt::Key_Up);
        QCOMPARE(w->searchPopup()->currentItem()->data(SearchSuggestPopup::KindRole).toInt(), int(SearchSuggestPopup::TagItem));
        QTest::keyClick(box(), Qt::Key_Return);                                  // choose the tag
        QCOMPARE(box()->text(), QString(""));                                    // the tag replaces the query
        QVERIFY(!model()->isSearching());
        QCOMPARE(w->sidebarWidget()->filter().tagIds.size(), 1);
        QCOMPARE(model()->rowCount(), 2);                                        // both podman recordings
        QVERIFY(!w->searchPopup()->isVisible());
    }
    void clickingARecordingSuggestionSelectsItsCard()
    {
        startWindow();
        box()->setFocus();
        box()->setText("shopping");
        w->runSearchNow();
        auto* pop = w->searchPopup();
        QVERIFY(pop->isVisible());
        QListWidgetItem* rec = nullptr;
        for (int i = 0; i < pop->count(); ++i)
            if (pop->item(i)->data(SearchSuggestPopup::KindRole).toInt() == SearchSuggestPopup::RecordingItem) rec = pop->item(i);
        QVERIFY(rec);
        QSignalSpy sel(w->cardView(), &CardListView::currentRecordingChanged);
        QTest::mouseClick(pop->viewport(), Qt::LeftButton, {}, pop->visualItemRect(rec).center());
        QVERIFY(!sel.isEmpty());
        QCOMPARE(sel.last().at(0).toLongLong(), memo);
        QCOMPARE(box()->text(), QString(""));
        QVERIFY(!model()->isSearching());
    }
    void choosingATranscriptHitSelectsTheCardAndRequestsTheTranscript()
    {
        startWindow();
        QSignalSpy open(w.get(), &MainWindow::openTranscriptRequested);
        box()->setFocus();
        box()->setText("Quadlet");
        w->runSearchNow();
        auto* pop = w->searchPopup();
        QListWidgetItem* seg = nullptr;
        for (int i = 0; i < pop->count(); ++i)
            if (pop->item(i)->data(SearchSuggestPopup::KindRole).toInt() == SearchSuggestPopup::SegmentItem) seg = pop->item(i);
        QVERIFY(seg);
        pop->setCurrentItem(seg);
        QTest::keyClick(box(), Qt::Key_Return);
        QCOMPARE(open.count(), 1);
        QCOMPARE(open.at(0).at(0).toLongLong(), study);
        QCOMPARE(open.at(0).at(1).toLongLong(), qint64(6000));                   // third segment of the study recording
        QCOMPARE(w->cardView()->currentIndex().data(RecordingListModel::IdRole).toLongLong(), study);
    }
    void escapeClosesSuggestionsThenClearsTheSearch()
    {
        startWindow();
        box()->setFocus();
        box()->setText("podman");
        w->runSearchNow();
        QVERIFY(w->searchPopup()->isVisible());
        QTest::keyClick(box(), Qt::Key_Escape);
        QVERIFY(!w->searchPopup()->isVisible());
        QCOMPARE(box()->text(), QString("podman"));                              // first Esc keeps the query
        QVERIFY(model()->isSearching());
        QTest::keyClick(box(), Qt::Key_Escape);
        QCOMPARE(box()->text(), QString(""));                                    // second Esc clears it
        QVERIFY(!model()->isSearching());
        QCOMPARE(model()->rowCount(), 4);
    }
    void emptyingTheBoxRestoresTheListImmediately()
    {
        startWindow();
        box()->setText("牛乳");
        w->runSearchNow();
        QCOMPARE(shown(), QList<qint64>({ memo }));
        box()->clear();
        QCOMPARE(model()->rowCount(), 4);
        QCOMPARE(headerTitle(), QString("すべて"));
        box()->setText("   ");                                                     // whitespace only is "no search"
        QTest::qWait(MainWindow::kSearchDelayMs + 100);
        QVERIFY(!model()->isSearching());
    }
    void noMatchShowsAnEmptyListAndNoSuggestions()
    {
        startWindow();
        box()->setFocus();
        box()->setText("kubernetes");
        w->runSearchNow();
        QCOMPARE(model()->rowCount(), 0);
        QVERIFY2(headerCount().contains("該当なし"), qPrintable(headerCount()));
        QVERIFY(!w->searchPopup()->isVisible());
    }
    void sidebarFiltersAndSearchCombine()
    {
        startWindow();
        box()->setText("podman");
        w->runSearchNow();
        QCOMPARE(model()->rowCount(), 2);
        RecordingFilter f; f.scope = RecordingFilter::Scope::Folder; f.folderId = 1;
        // restrict to recordings that carry the "docker" tag
        qint64 dockerTag = 0;
        for (const auto& t : w->sidebarWidget()->data().tags) if (t.name.toLower() == "docker") dockerTag = t.id;
        if (dockerTag) { f.tagIds = { dockerTag }; w->sidebarWidget()->setFilter(f); }
        const int both = model()->rowCount();
        QVERIFY(both <= 2);
        QVERIFY(model()->isSearching());                                         // the search survives the filter change
        QVERIFY2(headerCount().contains("「podman」"), qPrintable(headerCount()));
        if (dockerTag) QVERIFY2(headerCount().contains("#"), qPrintable(headerCount()));
        for (qint64 id : shown()) QVERIFY(id == docker || id == study);
    }
    void searchResultsSurviveRowRefreshes()
    {
        startWindow();
        box()->setText("podman");
        w->runSearchNow();
        QSqlQuery q(db.handle());
        QVERIFY(q.exec(QString("UPDATE recordings SET favorite=1 WHERE id=%1").arg(docker)));
        QSignalSpy reset(model(), &QAbstractItemModel::modelReset);
        model()->refreshRecording(docker);                                       // what a finished job triggers
        QCOMPARE(reset.count(), 0);
        QCOMPARE(model()->rowCount(), 2);
        const QModelIndex i = model()->index(model()->rowForId(docker), 0);
        QVERIFY(i.data(RecordingListModel::FavoriteRole).toBool());
        QVERIFY(!i.data(RecordingListModel::SnippetRole).toString().isEmpty());  // still a search row
    }
    void ctrlKFocusesTheSearchBoxAndSelectsItsText()
    {
        startWindow();
        box()->setText("abc");
        w->cardView()->setFocus();
        QVERIFY(!box()->hasFocus());
        QTest::keyClick(w.get(), Qt::Key_K, Qt::ControlModifier);
        QVERIFY(box()->hasFocus());
        QCOMPARE(box()->selectedText(), QString("abc"));
    }
    void withoutATokenizerTypingIsHarmless()
    {
        startWindow(false);
        box()->setFocus();
        QTest::keyClicks(box(), "podman");
        QTest::qWait(MainWindow::kSearchDelayMs + 100);
        QVERIFY(!model()->isSearching());
        QCOMPARE(model()->rowCount(), 4);
        QTest::keyClick(box(), Qt::Key_Return);
        QTest::keyClick(box(), Qt::Key_Escape);
    }
};

QTEST_MAIN(TstSearchUi)
#include "tst_search_ui.moc"
