#include <QAction>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QSqlError>
#include <QSqlQuery>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>

#include "core/Database.h"
#include "core/ITokenizer.h"
#include "core/KnowledgeBuilder.h"
#include "core/TranscriptRepository.h"
#include "ui/CardDelegate.h"
#include "ui/CardListView.h"
#include "ui/DetailPane.h"
#include "ui/MainWindow.h"
#include "ui/MiniPlayer.h"
#include "ui/PlaybackController.h"
#include "ui/RecordingListModel.h"
#include "ui/TranscriptModel.h"
#include "ui/TranscriptView.h"
#include "FakePlayer.h"

using namespace ake;
using State = IAudioPlayer::State;

class TstPlayerUi : public QObject {
    Q_OBJECT
    Database db;
    QTemporaryDir dir;
    std::unique_ptr<ITokenizer> tok;
    std::unique_ptr<FakePlayer> fake;
    std::unique_ptr<MainWindow> w;
    qint64 meeting = 0, memo = 0, gone = 0;
    int m_n = 0;

    qint64 add(const QString& name, const QStringList& sentences, bool file = true)
    {
        const QString path = dir.filePath(name);
        if (file) { QFile f(path); f.open(QIODevice::WriteOnly); f.write("x"); }
        QSqlQuery q(db.handle());
        q.prepare("INSERT INTO recordings(folder_id,rel_path,abs_path,status,recorded_at,created_at,updated_at,duration_ms) VALUES(1,?,?,'done',?,0,0,20000)");
        q.addBindValue(name); q.addBindValue(path); q.addBindValue(1700000000 + m_n++ * 100);
        if (!q.exec()) qFatal("%s", qPrintable(q.lastError().text()));
        const qint64 id = q.lastInsertId().toLongLong();
        TranscriptResult r; r.language = "ja"; r.model = "t";
        qint64 t = 0;
        for (const QString& s : sentences) { r.segments.append({ t, t + 3800, s }); t += 4000; }
        if (!TranscriptRepository(db).save(id, r)) qFatal("save");
        if (!KnowledgeBuilder(db, *tok).build(id)) qFatal("build");
        return id;
    }
    void start(bool withPlayer = true)
    {
        fake = std::make_unique<FakePlayer>();
        w = std::make_unique<MainWindow>();
        w->setTokenizer(tok.get());
        w->setDatabase(&db);
        if (withPlayer) w->setAudioPlayer(fake.get());
        w->show();
        QVERIFY(QTest::qWaitForWindowActive(w.get()));
    }
    RecordingListModel* model() { return w->recordingModel(); }
    QModelIndex idx(qint64 id) { return model()->index(model()->rowForId(id), 0); }
    void clickPlayOnCard(qint64 id)
    {
        CardListView* v = w->cardView();
        v->scrollTo(idx(id));
        QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, CardDelegate::actionButtonRect(v->visualRect(idx(id)), CardAction::Play).center());
    }
    bool isPlayingCard(qint64 id) { return idx(id).data(RecordingListModel::PlayingRole).toBool(); }

private slots:
    void init()
    {
        tok = createTokenizer();
        QVERIFY(db.open(":memory:"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("INSERT INTO folders(id,path) VALUES(1,'/f')"));
        m_n = 0;
        meeting = add("meeting.m4a", { "最初の発言です。", "二番目の発言です。", "三番目の発言です。", "四番目の発言です。", "五番目の発言です。" });
        memo = add("memo.wav", { "牛乳を買います。" });
        gone = add("gone.mp3", { "ファイルのない録音です。" }, false);
    }
    void cleanup() { w.reset(); fake.reset(); }

    // ---- mini player
    void hiddenUntilARecordingIsLoadedThenShowsEverything()
    {
        start();
        auto* mp = w->miniPlayer();
        QVERIFY(!mp->isVisible());
        QVERIFY(w->playback()->play(meeting, 4000));
        QVERIFY(mp->isVisible());
        QCOMPARE(mp->titleText(), w->playback()->title());
        QCOMPARE(mp->playButton()->text(), QString("❚❚"));              // playing: the button offers pause
        QCOMPARE(mp->timeText(), QString("00:04 / 00:20"));
        QCOMPARE(mp->seekSlider()->maximum(), 20000);
        QCOMPARE(mp->seekSlider()->value(), 4000);
        QCOMPARE(mp->sentenceText(), QString("00:00:04 ・ 二番目の発言です。"));
        fake->advance(4000);
        QCOMPARE(mp->timeText(), QString("00:08 / 00:20"));
        QCOMPARE(mp->seekSlider()->value(), 8000);
        QCOMPARE(mp->sentenceText(), QString("00:00:08 ・ 三番目の発言です。"));
    }
    void buttonsPlayPauseSkipAndClose()
    {
        start();
        auto* mp = w->miniPlayer();
        QVERIFY(w->playback()->play(meeting, 10000));
        QTest::mouseClick(mp->playButton(), Qt::LeftButton);
        QCOMPARE(fake->state(), State::Paused);
        QCOMPARE(mp->playButton()->text(), QString("▶"));
        QTest::mouseClick(mp->playButton(), Qt::LeftButton);
        QCOMPARE(fake->state(), State::Playing);
        QTest::mouseClick(mp->forwardButton(), Qt::LeftButton);
        QCOMPARE(fake->position(), qint64(15000));
        QTest::mouseClick(mp->backButton(), Qt::LeftButton);
        QTest::mouseClick(mp->backButton(), Qt::LeftButton);
        QCOMPARE(fake->position(), qint64(5000));
        QTest::mouseClick(mp->closeButton(), Qt::LeftButton);
        QVERIFY(!mp->isVisible());
        QCOMPARE(w->playback()->recordingId(), qint64(0));
        QCOMPARE(fake->state(), State::Stopped);
    }
    void whenTheRecordingEndsThePlayerStaysAndOffersPlayAgain()
    {
        start();
        auto* mp = w->miniPlayer();
        QVERIFY(w->playback()->play(meeting, 18000));
        fake->advance(5000);
        QVERIFY(mp->isVisible());
        QCOMPARE(mp->playButton()->text(), QString("▶"));
        QCOMPARE(mp->timeText(), QString("00:00 / 00:20"));
        QTest::mouseClick(mp->playButton(), Qt::LeftButton);
        QCOMPARE(fake->state(), State::Playing);
        QCOMPARE(fake->loads, 1);                                         // replay without reloading
    }
    void draggingTheSeekBarSeeksOnReleaseOnly()
    {
        start();
        auto* mp = w->miniPlayer();
        QVERIFY(w->playback()->play(meeting, 2000));
        fake->seeks.clear();
        QSlider* s = mp->seekSlider();
        QMetaObject::invokeMethod(s, "sliderPressed");                    // the user grabs the knob
        QVERIFY(mp->isDragging());
        fake->advance(1000);                                              // playback moves on, the knob must not jump back
        QCOMPARE(s->value(), 2000);
        s->setValue(12000);                                               // dragged to 12 s
        QMetaObject::invokeMethod(s, "sliderMoved", Q_ARG(int, 12000));
        QCOMPARE(mp->timeText(), QString("00:12 / 00:20"));              // the target time is shown while dragging
        QVERIFY(fake->seeks.isEmpty());                                   // ... but no seek yet
        QMetaObject::invokeMethod(s, "sliderReleased");
        QVERIFY(!mp->isDragging());
        QCOMPARE(fake->seeks.size(), 1);
        QCOMPARE(fake->seeks.last(), qint64(12000));
    }
    void speedAndVolumeControlsReachThePlayer()
    {
        start();
        auto* mp = w->miniPlayer();
        QVERIFY(w->playback()->play(meeting, 0));
        QCOMPARE(mp->speedButton()->text(), QString("1.0x ▾"));
        QAction* fast = nullptr;
        for (QAction* a : mp->speedButton()->menu()->actions()) if (a->text() == "1.50x") fast = a;
        QVERIFY(fast);
        fast->trigger();
        QCOMPARE(fake->rate(), 1.5);
        QCOMPARE(mp->speedButton()->text(), QString("1.50x ▾"));
        mp->volumeSlider()->setValue(25);
        QCOMPARE(fake->volume(), 0.25);
    }
    void theSpeedControlIsHiddenWhenTheBackendCannotChangeSpeed()
    {
        fake = std::make_unique<FakePlayer>();
        fake->rateSupported = false;
        w = std::make_unique<MainWindow>();
        w->setTokenizer(tok.get());
        w->setDatabase(&db);
        w->setAudioPlayer(fake.get());
        w->show();
        QVERIFY(QTest::qWaitForWindowActive(w.get()));
        QVERIFY(w->playback()->play(meeting, 0));
        QVERIFY(w->miniPlayer()->isVisible());
        QVERIFY(!w->miniPlayer()->speedButton()->isVisible());
        QVERIFY(w->miniPlayer()->volumeSlider()->isVisible());           // the rest of the controls are there
        // and a backend that can: shown
        cleanup();
        start();
        QVERIFY(w->playback()->play(meeting, 0));
        QVERIFY(w->miniPlayer()->speedButton()->isVisible());
    }
    void errorsAreShownInThePlayerAndTheStatusBar()
    {
        add("corrupt.flac", { "壊れたファイルの録音です。" });
        // a file the backend cannot decode: the fake reports it asynchronously, like the real one
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("SELECT id FROM recordings WHERE rel_path='corrupt.flac'"));
        QVERIFY(q.next());
        const qint64 corrupt = q.value(0).toLongLong();
        start();
        QVERIFY(w->playback()->play(corrupt, 0));
        QTRY_VERIFY(!w->miniPlayer()->errorText().isEmpty());
        QVERIFY2(w->miniPlayer()->errorText().contains("ストリーム"), qPrintable(w->miniPlayer()->errorText()));
        QVERIFY(w->statusBar()->currentMessage().contains("再生できません"));
    }

    // ---- cards
    void clickingAPlayButtonPlaysThenPausesAndMarksTheCard()
    {
        start();
        QSignalSpy asked(w.get(), &MainWindow::playRequested);
        QVERIFY(!isPlayingCard(meeting));
        clickPlayOnCard(meeting);
        QCOMPARE(asked.count(), 1);
        QCOMPARE(w->playback()->recordingId(), meeting);
        QVERIFY(fake->state() == State::Playing);
        QVERIFY(isPlayingCard(meeting));
        QVERIFY(w->miniPlayer()->isVisible());
        QCOMPARE(w->miniPlayer()->titleText(), idx(meeting).data(RecordingListModel::TitleRole).toString());
        clickPlayOnCard(meeting);                                         // the same card again: pause
        QCOMPARE(fake->state(), State::Paused);
        QVERIFY(!isPlayingCard(meeting));
        clickPlayOnCard(meeting);
        QCOMPARE(fake->state(), State::Playing);
        QCOMPARE(fake->loads, 1);
        clickPlayOnCard(memo);                                            // another card: switch
        QCOMPARE(w->playback()->recordingId(), memo);
        QVERIFY(isPlayingCard(memo));
        QVERIFY(!isPlayingCard(meeting));
        QCOMPARE(fake->loads, 2);
    }
    void thePlayingCardKeepsItsButtonVisibleWithoutHover()
    {
        start();
        clickPlayOnCard(meeting);
        QTest::mouseMove(w.get(), QPoint(5, 5));                          // pointer away from the cards
        w->cardView()->setCurrentIndex(idx(memo));                        // and another card selected
        const QImage img = w->cardView()->viewport()->grab().toImage();
        const QRect b = CardDelegate::actionButtonRect(w->cardView()->visualRect(idx(meeting)), CardAction::Play);
        const QColor c = img.pixelColor(b.left() + 2, b.center().y());      // the button's accent colour is drawn
        QVERIFY2(c.blue() > c.red(), qPrintable(c.name()));
    }
    void aMissingAudioFileIsReportedWithoutBreakingAnything()
    {
        start();
        clickPlayOnCard(meeting);
        clickPlayOnCard(gone);                                            // its file does not exist
        QVERIFY2(w->statusBar()->currentMessage().contains("gone.mp3"), qPrintable(w->statusBar()->currentMessage()));
        QCOMPARE(w->playback()->recordingId(), meeting);                  // the other one keeps playing
        QVERIFY(fake->state() == State::Playing);
        QVERIFY(w->miniPlayer()->isVisible());
    }
    void spaceTogglesPlaybackOfTheSelectedCardButTypesInTheSearchBox()
    {
        start();
        w->cardView()->setFocus();
        w->cardView()->setCurrentIndex(idx(memo));
        QTest::keyClick(w->cardView(), Qt::Key_Space);
        QCOMPARE(w->playback()->recordingId(), memo);
        QCOMPARE(fake->state(), State::Playing);
        QTest::keyClick(w->cardView(), Qt::Key_Space);
        QCOMPARE(fake->state(), State::Paused);
        w->searchBox()->setFocus();
        QTest::keyClick(w->searchBox(), Qt::Key_A);
        QTest::keyClick(w->searchBox(), Qt::Key_Space);
        QTest::keyClick(w->searchBox(), Qt::Key_B);
        QCOMPARE(w->searchBox()->text(), QString("a b"));                 // a space in the search box is a space
        QCOMPARE(fake->state(), State::Paused);
    }
    void detailPaneButtonPlaysAndPauses()
    {
        start();
        w->cardView()->setCurrentIndex(idx(meeting));
        QTest::mouseClick(w->detailPaneWidget()->playButton(), Qt::LeftButton);
        QCOMPARE(w->playback()->recordingId(), meeting);
        QCOMPARE(fake->state(), State::Playing);
        QTest::mouseClick(w->detailPaneWidget()->playButton(), Qt::LeftButton);
        QCOMPARE(fake->state(), State::Paused);
    }

    // ---- transcript
    void clickingASentenceInTheTranscriptPlaysFromThere()
    {
        start();
        w->showTranscript(meeting);
        auto* tv = w->transcriptView();
        QListView* list = tv->list();
        auto clickRow = [&](int row) { QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualRect(tv->model()->index(row, 0)).center()); };
        clickRow(2);                                                       // third sentence: starts at 8 s
        QCOMPARE(w->playback()->recordingId(), meeting);
        QCOMPARE(fake->state(), State::Playing);
        QCOMPARE(fake->position(), qint64(8000));
        QCOMPARE(fake->loads, 1);
        clickRow(4);                                                       // another sentence of the same recording: only a seek
        QCOMPARE(fake->position(), qint64(16000));
        QCOMPARE(fake->loads, 1);
    }
    void thePlayingSentenceIsHighlightedAndFollowedInTheTranscript()
    {
        start();
        w->showTranscript(meeting);
        auto* tv = w->transcriptView();
        QVERIFY(w->playback()->play(meeting, 0));
        QCOMPARE(tv->playbackRow(), 0);
        fake->advance(4500);
        QCOMPARE(tv->playbackRow(), 1);
        fake->advance(8000);
        QCOMPARE(tv->playbackRow(), 3);
        // another recording plays: the highlight in this transcript is dropped
        QVERIFY(w->playback()->play(memo, 0));
        QCOMPARE(tv->playbackRow(), -1);
    }
    void openingTheTranscriptOfThePlayingRecordingHighlightsItAtOnce()
    {
        start();
        QVERIFY(w->playback()->play(meeting, 9000));
        w->showTranscript(meeting);
        QCOMPARE(w->transcriptView()->playbackRow(), 2);
        w->showLibrary();
        w->showTranscript(memo);                                           // not the playing one
        QCOMPARE(w->transcriptView()->playbackRow(), -1);
    }
    void spaceInTheTranscriptTogglesPlayback()
    {
        start();
        w->showTranscript(meeting);
        auto* list = w->transcriptView()->list();
        QTest::keyClick(list, Qt::Key_Space);                              // nothing playing yet: starts this recording
        QCOMPARE(w->playback()->recordingId(), meeting);
        QCOMPARE(fake->state(), State::Playing);
        QTest::keyClick(list, Qt::Key_Space);
        QCOMPARE(fake->state(), State::Paused);
        QTest::keyClick(list, Qt::Key_Space);
        QCOMPARE(fake->state(), State::Playing);
    }

    // ---- lifetime / absence
    void closingTheWindowStopsThePlayback()
    {
        start();
        clickPlayOnCard(meeting);
        QCOMPARE(fake->state(), State::Playing);
        const int stopsBefore = fake->stops;
        w.reset();
        QVERIFY(fake->stops > stopsBefore);
        QCOMPARE(fake->state(), State::Stopped);
    }
    void withoutAPlayerTheButtonsAreHarmless()
    {
        start(false);
        QSignalSpy asked(w.get(), &MainWindow::playRequested);
        clickPlayOnCard(meeting);
        QCOMPARE(asked.count(), 1);                                       // the request is still announced
        QVERIFY(w->playback() == nullptr);
        QVERIFY(!w->miniPlayer()->isVisible());
        w->cardView()->setFocus();
        QTest::keyClick(w->cardView(), Qt::Key_Space);                    // Space does nothing and does not crash
        w->showTranscript(meeting);
        QTest::mouseClick(w->transcriptView()->list()->viewport(), Qt::LeftButton, {}, w->transcriptView()->list()->visualRect(w->transcriptView()->model()->index(1, 0)).center());
    }
};

QTEST_MAIN(TstPlayerUi)
#include "tst_player_ui.moc"
