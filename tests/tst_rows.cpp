#include <QAction>
#include <QFile>
#include <QLineEdit>
#include <QMenu>
#include <QTemporaryDir>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QToolButton>
#include <QtTest>

#include "core/Database.h"
#include "core/DemoData.h"
#include "core/ITokenizer.h"
#include "core/JobScheduler.h"
#include "core/SearchService.h"
#include "core/SqlText.h"
#include "ui/CardListView.h"
#include "ui/DetailPane.h"
#include "ui/PlaybackController.h"
#include "ui/MainWindow.h"
#include "ui/RecordingListModel.h"
#include "ui/RowDelegate.h"
#include "ui/Theme.h"
#include "ui/TranscriptView.h"
#include "FakePlayer.h"
#include "Fakes.h"

using namespace ake;
using Mode = CardListView::Mode;

namespace {
void doubleClick(QWidget* vp, const QPoint& pos)       // see tst_transcript_ui: the sequence a real double click produces
{
    QTest::mouseClick(vp, Qt::LeftButton, {}, pos);
    QMouseEvent dbl(QEvent::MouseButtonDblClick, pos, vp->mapToGlobal(pos), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(vp, &dbl);
    QMouseEvent rel(QEvent::MouseButtonRelease, pos, vp->mapToGlobal(pos), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(vp, &rel);
}
} // namespace

class TstRows : public QObject {
    Q_OBJECT
    Database db;
    std::unique_ptr<RecordingListModel> model;
    std::unique_ptr<CardListView> view;

    QModelIndex rowOf(const QString& title)
    {
        for (int r = 0; r < model->rowCount(); ++r)
            if (model->index(r, 0).data(RecordingListModel::TitleRole).toString() == title) return model->index(r, 0);
        return {};
    }
    QColor pixel(const QPoint& p) { return view->viewport()->grab().toImage().pixelColor(p); }
    void openView(int w = 1300, int h = 700)
    {
        model = std::make_unique<RecordingListModel>(db);
        view = std::make_unique<CardListView>();
        view->setModel(model.get());
        view->setTheme(Theme::light());
        view->resize(w, h);
        view->show();
        QVERIFY(QTest::qWaitForWindowExposed(view.get()));
    }

private slots:
    void init()
    {
        QVERIFY(db.open(":memory:"));
        QVERIFY(seedDemoData(db));
        // seedDemoData marks one recording 'transcribing' and one 'queued' with fake jobs; that is what we want to see
    }
    void cleanup() { view.reset(); model.reset(); }

    // ---- column geometry (pure)
    void columnsNeverOverlapAndStayInsideTheRow()
    {
        for (int w = 520; w <= 2200; w += 37) {
            const auto c = RowDelegate::columns(w);
            QVERIFY2(c.textW >= 80, qPrintable(QString::number(w)));
            QVERIFY(c.textX + c.textW <= (c.showTags ? c.tagsX : c.dateX));
            if (c.showTags) QVERIFY(c.tagsX + c.tagsW <= c.dateX);
            QVERIFY(c.dateX + c.dateW <= c.durX);
            QVERIFY(c.durX + c.durW <= c.actionX);
            QVERIFY(c.actionX + c.actionW <= w);
            QVERIFY(c.actionX + c.actionW >= w - 20);                    // right-aligned with a small margin
            QCOMPARE(c.showTags, w >= RowDelegate::kTagsMinWidth);
            for (CardAction a : { CardAction::Play, CardAction::Retry, CardAction::Analyze }) {
                const QRect b = RowDelegate::actionButtonRect(QRect(0, 100, w, RowDelegate::kRowHeight), a);
                QVERIFY(QRect(c.actionX, 100, c.actionW, RowDelegate::kRowHeight).contains(b));
            }
        }
        // wider rows give the title more room, never less
        QVERIFY(RowDelegate::columns(1600).textW > RowDelegate::columns(1000).textW);
    }

    // ---- switching modes
    void rowsAreAFullWidthStackAndCardsAreAGrid()
    {
        openView(1300, 700);
        QCOMPARE(view->mode(), Mode::Cards);
        QVERIFY(!view->headerWidget()->isVisible());
        const QRect card0 = view->visualRect(model->index(0, 0)), card1 = view->visualRect(model->index(1, 0));
        QVERIFY(card1.left() > card0.right() - 5);                      // side by side
        QSignalSpy changed(view.get(), &CardListView::modeChanged);
        view->setMode(Mode::Rows);
        view->setMode(Mode::Rows);                                       // same mode again: no signal
        QCOMPARE(changed.count(), 1);
        QCOMPARE(view->viewMode(), QListView::ListMode);
        QVERIFY(view->headerWidget()->isVisible());
        QCOMPARE(view->headerWidget()->height(), RowDelegate::kHeaderHeight);
        QCOMPARE(view->headerWidget()->width(), view->viewport()->width());
        const QRect r0 = view->visualRect(model->index(0, 0)), r1 = view->visualRect(model->index(1, 0));
        QCOMPARE(r0.height(), RowDelegate::kRowHeight);
        QCOMPARE(r0.width(), view->viewport()->width());                 // rows span the whole width
        QCOMPARE(r1.top(), r0.bottom() + 1);                             // stacked with no gap
        QCOMPARE(r1.left(), r0.left());
        view->setMode(Mode::Cards);
        QVERIFY(!view->headerWidget()->isVisible());
        QCOMPARE(view->viewMode(), QListView::IconMode);
        QCOMPARE(view->visualRect(model->index(0, 0)).size(), QSize(CardDelegate::kCardWidth + 2 * CardDelegate::kMargin, CardDelegate::kCardHeight + 2 * CardDelegate::kMargin));
        QCOMPARE(changed.count(), 2);
    }
    void selectionAndScrollSurviveSwitching()
    {
        openView(1300, 300);                                             // short window: not everything is visible
        const QModelIndex last = model->index(model->rowCount() - 1, 0);
        view->setCurrentIndex(last);
        view->scrollTo(last);
        view->setMode(Mode::Rows);
        QCOMPARE(view->currentIndex(), last);
        QVERIFY2(view->viewport()->rect().intersects(view->visualRect(last)), "the selected row must be scrolled into view");
        view->setMode(Mode::Cards);
        QCOMPARE(view->currentIndex(), last);
        QVERIFY(view->viewport()->rect().intersects(view->visualRect(last)));
    }
    void rowsFollowTheViewWidthOnResize()
    {
        openView(1300, 600);
        view->setMode(Mode::Rows);
        view->resize(900, 600);
        QTest::qWait(30);
        QCOMPARE(view->visualRect(model->index(0, 0)).width(), view->viewport()->width());
        QCOMPARE(view->headerWidget()->width(), view->viewport()->width());
        view->resize(1500, 600);
        QTest::qWait(30);
        QCOMPARE(view->visualRect(model->index(0, 0)).width(), view->viewport()->width());
    }

    // ---- painting
    void statusDotsAndSelectionUseTheTheme()
    {
        for (const Theme& t : { Theme::light(), Theme::dark() }) {
            openView(1300, 700);
            view->setTheme(t);
            view->setMode(Mode::Rows);
            const auto c = RowDelegate::columns(view->viewport()->width());
            auto dotAt = [&](const QString& title) {
                const QRect r = view->visualRect(rowOf(title));
                return pixel(QPoint(c.dotX, r.top() + 22));
            };
            QCOMPARE(dotAt("Docker移行会議"), t.green);                  // done
            QCOMPARE(dotAt("interview_04.mp3").name(), t.accent.name()); // running (demo data: transcribing)
            QCOMPARE(dotAt("broken_rec.flac"), t.red);                   // error
            // the selected row gets the selection colour
            view->setCurrentIndex(rowOf("Docker移行会議"));
            const QRect sel = view->visualRect(rowOf("Docker移行会議"));
            QCOMPARE(pixel(QPoint(3, sel.top() + 30)), t.accentBg);
            view.reset(); model.reset();
        }
    }
    void theDonePlayButtonShowsOnlyForHoverSelectionOrPlayback()
    {
        openView(1300, 700);
        view->setMode(Mode::Rows);
        const QModelIndex done = rowOf("新規サービス企画ミーティング");
        const QRect b = view->actionRect(done, CardAction::Play);
        const QPoint probe(b.left() + 3, b.center().y());
        const QColor idle = pixel(probe);
        QVERIFY2(!(idle.blue() > idle.red() + 60), qPrintable("no button while idle: " + idle.name()));
        view->setCurrentIndex(done);                                      // selected: the accent-coloured button appears
        const QColor selected = pixel(probe);
        QVERIFY2(selected.blue() > selected.red() + 60, qPrintable(selected.name()));
        view->setCurrentIndex(rowOf("Docker移行会議"));                   // select another row ...
        model->setPlaying(model->data(done, RecordingListModel::IdRole).toLongLong(), true);   // ... and "play" this one
        const QColor playing = pixel(probe);
        QVERIFY2(playing.blue() > playing.red() + 60, "the playing row keeps its button");
    }
    void retryAndAnalyzeButtonsAreAlwaysVisibleOnRowsThatNeedThem()
    {
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("UPDATE recordings SET status='new' WHERE rel_path='memos/queued_note.wav'"));
        openView(1300, 700);
        view->setMode(Mode::Rows);
        for (auto [title, action] : { std::pair<QString, CardAction>{ "broken_rec.flac", CardAction::Retry }, { "queued_note.wav", CardAction::Analyze } }) {
            const QModelIndex i = rowOf(title);
            const QRect b = view->actionRect(i, action);
            QVERIFY2(b.isValid() && view->viewport()->rect().contains(b), qPrintable(title));
            const QColor edge = pixel(QPoint(b.left() + 1, b.center().y()));            // the ghost button's border line
            QVERIFY2(edge.name() != Theme::light().accentBg.name(), "the button border is drawn");
            const QColor inside = pixel(b.center() + QPoint(-20, -8));
            QVERIFY2(inside == Theme::light().panel || inside.lightness() > 200, qPrintable(title + " " + inside.name()));
        }
    }
    void tagsColumnAppearsWhenThereIsRoomAndShowsTheSearchBadge()
    {
        openView(1400, 600);
        view->setMode(Mode::Rows);
        const auto wide = RowDelegate::columns(view->viewport()->width());
        QVERIFY(wide.showTags);
        const QRect r = view->visualRect(rowOf("Docker移行会議"));
        // a chip is drawn (chip colour) at the start of the tags column
        QCOMPARE(pixel(QPoint(wide.tagsX + 6, r.top() + RowDelegate::kRowHeight / 2)), Theme::light().chip);
        // narrow: the column is dropped
        view->resize(760, 600);
        QTest::qWait(30);
        QVERIFY(!RowDelegate::columns(view->viewport()->width()).showTags);
    }
    void searchSnippetsAreHighlightedInRowsToo()
    {
        const auto tok = createTokenizer();
        // give a demo recording a transcript so that the search has something to find
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("SELECT id FROM recordings WHERE rel_path='meetings/docker.m4a'"));
        QVERIFY(q.next());
        SearchHit hit;
        hit.recordingId = q.value(0).toLongLong();
        hit.snippet = QStringLiteral("今日はPodmanへの移行について議論します");
        hit.highlights = { 3, 6 };                                       // "Podman"
        hit.segmentHits = 4;
        SearchResult res;
        res.hits << hit;
        openView(1400, 600);
        view->setMode(Mode::Rows);
        model->setSearchResult(res);
        QCOMPARE(model->rowCount(), 1);
        const QRect r = view->visualRect(model->index(0, 0));
        const auto c = RowDelegate::columns(view->viewport()->width());
        const QImage img = view->viewport()->grab().toImage();
        // somewhere on the summary line there are highlight-coloured pixels
        bool foundHighlight = false;
        for (int x = c.textX; x < c.textX + 260 && !foundHighlight; ++x)
            for (int y = r.top() + 33; y < r.top() + 53; ++y)
                if (img.pixelColor(x, y) == Theme::light().hl) { foundHighlight = true; break; }
        QVERIFY2(foundHighlight, "the matched word must be highlighted in the row's summary line");
        // and the "N箇所一致" badge (also highlight-coloured) starts the tags column
        QCOMPARE(img.pixelColor(c.tagsX + 4, r.top() + RowDelegate::kRowHeight / 2), Theme::light().hl);
    }

    // ---- clicks
    void clickingTheActionsEmitsThemAndOtherClicksDoNot()
    {
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("UPDATE recordings SET status='new' WHERE rel_path='memos/queued_note.wav'"));
        openView(1300, 700);
        view->setMode(Mode::Rows);
        QSignalSpy spy(view.get(), &CardListView::cardActionRequested);
        auto click = [&](const QString& title, CardAction a) {
            const QModelIndex i = rowOf(title);
            view->scrollTo(i);
            QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, view->actionRect(i, a).center());
        };
        click("Docker移行会議", CardAction::Play);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.last().at(1).value<CardAction>(), CardAction::Play);
        click("broken_rec.flac", CardAction::Retry);
        QCOMPARE(spy.last().at(1).value<CardAction>(), CardAction::Retry);
        click("queued_note.wav", CardAction::Analyze);
        QCOMPARE(spy.last().at(1).value<CardAction>(), CardAction::Analyze);
        const int n = spy.count();
        const QRect body = view->visualRect(rowOf("Podman勉強会 #3"));
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, QPoint(body.left() + 300, body.center().y()));
        QCOMPARE(spy.count(), n);                                         // clicking the row itself only selects it
        QCOMPARE(view->currentIndex().data(RecordingListModel::TitleRole).toString(), QString("Podman勉強会 #3"));
    }

    // ---- scale
    void aLargeLibrarySwitchesAndScrollsQuickly()
    {
        QSqlQuery q(db.handle());
        QVERIFY(db.handle().transaction());
        q.prepare("INSERT INTO recordings(folder_id,rel_path,abs_path,status,recorded_at,created_at,updated_at,duration_ms) VALUES(1,?,?,'done',?,0,0,60000)");
        for (int i = 0; i < 3000; ++i) { q.addBindValue(QString("bulk/r%1.mp3").arg(i)); q.addBindValue(QString("/b/r%1.mp3").arg(i)); q.addBindValue(1600000000 + i); QVERIFY(q.exec()); }
        QVERIFY(db.handle().commit());
        openView(1300, 700);
        QCOMPARE(model->rowCount(), 3009);
        QElapsedTimer t; t.start();
        view->setMode(Mode::Rows);
        const qint64 toRows = t.elapsed();
        // not a vacuous measurement: the rows really occupy 3009 x 62 px of scrollable height
        QVERIFY2(view->verticalScrollBar()->maximum() > 3009 * RowDelegate::kRowHeight - 800, qPrintable(QString::number(view->verticalScrollBar()->maximum())));
        const QModelIndex lastRow = model->index(model->rowCount() - 1, 0);
        view->scrollTo(lastRow);
        QVERIFY(view->viewport()->rect().intersects(view->visualRect(lastRow)));
        view->scrollToTop();
        t.restart();
        view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
        view->viewport()->grab();
        const qint64 scrollMs = t.elapsed();
        t.restart();
        view->setMode(Mode::Cards);
        const qint64 toCards = t.elapsed();
        QVERIFY2(toRows < 800 && scrollMs < 500 && toCards < 1500,
                 qPrintable(QString("to rows %1 ms, scroll+paint %2 ms, to cards %3 ms").arg(toRows).arg(scrollMs).arg(toCards)));
        qInfo("3009 recordings: to rows %lld ms, scroll+paint %lld ms, to cards %lld ms", toRows, scrollMs, toCards);
    }
};

class TstRowsWindow : public QObject {
    Q_OBJECT
    Database db;
    fakes::FakeDecoder dec;
    std::unique_ptr<ITokenizer> tok;
    std::unique_ptr<MainWindow> w;
    std::unique_ptr<FakePlayer> fake;
    QTemporaryDir dir;
    qint64 idA = 0;

    qint64 add(const QString& name, int durationMs, qint64 when, const QString& status = "done")
    {
        const QString path = dir.filePath(name);
        QFile f(path); f.open(QIODevice::WriteOnly); f.write("x");
        QSqlQuery q(db.handle());
        q.prepare("INSERT INTO recordings(folder_id,rel_path,abs_path,status,error_msg,recorded_at,created_at,updated_at,duration_ms) VALUES(1,?,?,?,?,?,0,0,?)");
        q.addBindValue(name); q.addBindValue(path); q.addBindValue(status); q.addBindValue(sqlText(QString())); q.addBindValue(when); q.addBindValue(durationMs);
        if (!q.exec()) qFatal("%s", qPrintable(q.lastError().text()));
        return q.lastInsertId().toLongLong();
    }
    QStringList titles()
    {
        QStringList l;
        for (int r = 0; r < w->recordingModel()->rowCount(); ++r) l << w->recordingModel()->index(r, 0).data(RecordingListModel::TitleRole).toString();
        return l;
    }
    void start(bool sortBeforeDb = false)
    {
        fake = std::make_unique<FakePlayer>();
        w = std::make_unique<MainWindow>();
        w->setTokenizer(tok.get());
        if (sortBeforeDb) w->setSortOrder(RecordingListModel::SortOrder::Longest);
        w->setDatabase(&db);
        w->setAudioPlayer(fake.get());
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
        idA = add("a_old_long.mp3", 3600000, 1000);
        add("b_mid.mp3", 60000, 2000);
        add("c_new_short.mp3", 5000, 3000);
    }
    void cleanup() { w.reset(); fake.reset(); }

    void buttonsMenuAndShortcutsStayInSyncWithTheView()
    {
        start();
        auto* b1 = w->cardsViewButton(); auto* b2 = w->rowsViewButton();
        QVERIFY(b1->isChecked() && !b2->isChecked());
        QCOMPARE(b1->text(), QString("▦"));
        QCOMPARE(b2->text(), QString("☰"));
        QCOMPARE(w->cardView()->mode(), Mode::Cards);
        QTest::mouseClick(b2, Qt::LeftButton);
        QCOMPARE(w->cardView()->mode(), Mode::Rows);
        QVERIFY(b2->isChecked() && !b1->isChecked());
        QVERIFY(w->rowsViewAction()->isChecked());
        QTest::keyClick(w.get(), Qt::Key_1, Qt::ControlModifier);          // Ctrl+1: cards
        QCOMPARE(w->cardView()->mode(), Mode::Cards);
        QVERIFY(b1->isChecked() && !b2->isChecked());
        QTest::keyClick(w.get(), Qt::Key_2, Qt::ControlModifier);
        QCOMPARE(w->cardView()->mode(), Mode::Rows);
        w->cardsViewAction()->trigger();                                    // the 表示 menu
        QCOMPARE(w->cardView()->mode(), Mode::Cards);
        QVERIFY(b1->isChecked() && !b2->isChecked());
        QTest::mouseClick(b1, Qt::LeftButton);                              // clicking the already-active button keeps it active
        QVERIFY(b1->isChecked());
        w->setViewMode(Mode::Rows);                                         // programmatic: buttons follow
        QVERIFY(b2->isChecked() && !b1->isChecked());
    }
    void theSortMenuOrdersCardsAndRowsAlike()
    {
        start();
        QCOMPARE(titles(), QStringList({ "c_new_short.mp3", "b_mid.mp3", "a_old_long.mp3" }));
        QCOMPARE(w->sortButton()->text(), QString("新しい順 ▾"));
        auto choose = [&](const QString& text) {
            for (QAction* a : w->sortButton()->menu()->actions()) if (a->text() == text) { a->trigger(); return; }
            QFAIL(qPrintable("no sort action " + text));
        };
        choose("古い順");
        QCOMPARE(titles(), QStringList({ "a_old_long.mp3", "b_mid.mp3", "c_new_short.mp3" }));
        QCOMPARE(w->sortButton()->text(), QString("古い順 ▾"));
        choose("長い順");
        QCOMPARE(titles(), QStringList({ "a_old_long.mp3", "b_mid.mp3", "c_new_short.mp3" }));   // 1 h, 1 min, 5 s
        w->setViewMode(Mode::Rows);                                         // the same order in rows
        QCOMPARE(titles(), QStringList({ "a_old_long.mp3", "b_mid.mp3", "c_new_short.mp3" }));
        choose("新しい順");
        QCOMPARE(titles().first(), QString("c_new_short.mp3"));
        QVERIFY(w->sortButton()->menu()->actions().at(0)->isChecked());
    }
    void aSortChosenBeforeTheDatabaseIsAttachedApplies()
    {
        start(true);
        QCOMPARE(w->recordingModel()->sortOrder(), RecordingListModel::SortOrder::Longest);
        QCOMPARE(titles().first(), QString("a_old_long.mp3"));
        QCOMPARE(w->sortButton()->text(), QString("長い順 ▾"));
    }
    void rowsBehaveLikeCardsForSelectionDetailsPlaybackAndOpening()
    {
        start();
        w->setViewMode(Mode::Rows);
        auto* v = w->cardView();
        auto idx = [&](qint64 id) { return w->recordingModel()->index(w->recordingModel()->rowForId(id), 0); };
        // selecting a row fills the detail pane
        v->setCurrentIndex(idx(idA));
        QCOMPARE(w->detailPaneWidget()->currentId(), idA);
        // arrow keys move the selection and the detail pane follows
        v->setFocus();
        QTest::keyClick(v, Qt::Key_Up);
        QVERIFY(w->detailPaneWidget()->currentId() != idA);
        QTest::keyClick(v, Qt::Key_Down);
        QCOMPARE(w->detailPaneWidget()->currentId(), idA);
        // Space plays the selected row; the same play button on the row pauses it
        QTest::keyClick(v, Qt::Key_Space);
        QCOMPARE(w->playback()->recordingId(), idA);
        QVERIFY(fake->state() == IAudioPlayer::State::Playing);
        QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, v->actionRect(idx(idA), CardAction::Play).center());
        QVERIFY(fake->state() == IAudioPlayer::State::Paused);
        // double click opens the transcript (analysed recordings only): add one with a transcript
        QSqlQuery q(db.handle());
        QVERIFY(q.exec(QString("INSERT INTO transcripts(recording_id,language,model,created_at,is_current) VALUES(%1,'ja','m',0,1)").arg(idA)));
        QVERIFY(q.exec("INSERT INTO segments(transcript_id,idx,start_ms,end_ms,text) VALUES((SELECT max(id) FROM transcripts),0,0,1000,'こんにちは')"));
        doubleClick(v->viewport(), v->visualRect(idx(idA)).center() - QPoint(100, 0));
        QVERIFY(w->isTranscriptShown());
        QCOMPARE(w->transcriptView()->recordingId(), idA);
    }
    void searchResultsShowSnippetsInTheRows()
    {
        QSqlQuery q(db.handle());
        QVERIFY(q.exec(QString("INSERT INTO transcripts(recording_id,language,model,created_at,is_current) VALUES(%1,'ja','m',0,1)").arg(idA)));
        const qint64 tid = q.lastInsertId().toLongLong();
        const QString text = "今日はゼブラの話です";
        q.prepare("INSERT INTO segments(transcript_id,idx,start_ms,end_ms,text,wakati) VALUES(?,0,0,1000,?,?)");
        q.addBindValue(tid); q.addBindValue(text); q.addBindValue(createTokenizer()->wakati(text));   // the index the real pipeline would write
        QVERIFY(q.exec());
        QVERIFY(q.exec("INSERT INTO fts_segments(rowid,wakati) SELECT id, wakati FROM segments"));
        start();
        w->setViewMode(Mode::Rows);
        w->searchBox()->setText("ゼブラ");
        w->runSearchNow();
        QCOMPARE(w->recordingModel()->rowCount(), 1);
        const QModelIndex i = w->recordingModel()->index(0, 0);
        QVERIFY(i.data(RecordingListModel::SnippetRole).toString().contains("ゼブラ"));
        QVERIFY(!i.data(RecordingListModel::HighlightsRole).toList().isEmpty());
        w->searchBox()->clear();
        QCOMPARE(w->recordingModel()->rowCount(), 3);
        QVERIFY(w->cardView()->mode() == Mode::Rows);                        // clearing the search keeps the chosen view
    }
    void analyzeFromARowRunsTheJobAndTheRowUpdates()
    {
        const qint64 fresh = add("d_unanalysed.mp3", 8000, 4000, "new");
        JobScheduler sched(db, dec, fakes::factory());
        sched.setTokenizer(tok.get());
        fakes::g_stepMs = 15;
        start();
        w->setScheduler(&sched);
        w->setViewMode(Mode::Rows);
        auto* v = w->cardView();
        const QModelIndex i = w->recordingModel()->index(w->recordingModel()->rowForId(fresh), 0);
        QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, v->actionRect(i, CardAction::Analyze).center());
        QTRY_COMPARE_WITH_TIMEOUT(i.data(RecordingListModel::StateRole).value<CardState>(), CardState::Running, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(i.data(RecordingListModel::StateRole).value<CardState>(), CardState::Done, 10000);
        fakes::g_stepMs = 5;
    }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    int rc = 0;
    { TstRows t; rc |= QTest::qExec(&t, argc, argv); }
    { TstRowsWindow t; rc |= QTest::qExec(&t, argc, argv); }
    return rc;
}
#include "tst_rows.moc"
