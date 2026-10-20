#include <QFile>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include "core/Database.h"
#include "core/ITokenizer.h"
#include "core/KnowledgeBuilder.h"
#include "core/RecordingEditor.h"
#include "core/TranscriptRepository.h"
#include "ui/PlaybackController.h"
#include "FakePlayer.h"

using namespace ake;
using State = IAudioPlayer::State;
Q_DECLARE_METATYPE(ake::IAudioPlayer::State)

class TstPlayback : public QObject {
    Q_OBJECT
    Database db;
    QTemporaryDir dir;
    std::unique_ptr<FakePlayer> fake;       // a fresh player per test (QObjects cannot be assigned)
    std::unique_ptr<PlaybackController> pc;
    qint64 a = 0, b = 0, missing = 0, corrupt = 0;

    // a real (tiny) file on disk, because the controller checks that the audio file exists before loading
    qint64 add(const QString& name, const QStringList& sentences, bool createFile = true)
    {
        const QString path = dir.filePath(name);
        if (createFile) { QFile f(path); f.open(QIODevice::WriteOnly); f.write("x"); }
        QSqlQuery q(db.handle());
        q.prepare("INSERT INTO recordings(folder_id,rel_path,abs_path,status,recorded_at,created_at,updated_at,duration_ms) VALUES(1,?,?,'done',1700000000,0,0,20000)");
        q.addBindValue(name); q.addBindValue(path);
        if (!q.exec()) qFatal("%s", qPrintable(q.lastError().text()));
        const qint64 id = q.lastInsertId().toLongLong();
        if (!sentences.isEmpty()) {
            TranscriptResult r; r.language = "ja"; r.model = "t";
            qint64 t = 0;
            for (const QString& s : sentences) { r.segments.append({ t, t + 3800, s }); t += 4000; }
            if (!TranscriptRepository(db).save(id, r)) qFatal("save");
        }
        return id;
    }

private slots:
    void initTestCase() { qRegisterMetaType<ake::IAudioPlayer::State>(); }
    void init()
    {
        QVERIFY(db.open(":memory:"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("INSERT INTO folders(id,path) VALUES(1,'/f')"));
        pc.reset();
        fake = std::make_unique<FakePlayer>();
        pc = std::make_unique<PlaybackController>(db, *fake);
        a = add("meeting.m4a", { "最初の文です。", "二番目の文です。", "三番目の文です。", "四番目の文です。" });
        b = add("memo.wav", { "メモの文です。" });
        missing = add("gone.mp3", {}, false);
        corrupt = add("corrupt.flac", {});
    }

    void startsAtTheRequestedTimeAndReportsState()
    {
        QSignalSpy rec(pc.get(), &PlaybackController::recordingChanged), state(pc.get(), &PlaybackController::stateChanged);
        QCOMPARE(pc->recordingId(), qint64(0));
        QVERIFY(pc->play(a, 8000));
        QCOMPARE(pc->recordingId(), a);
        QCOMPARE(pc->title(), QString("meeting.m4a"));                    // no AI title: the file name
        QCOMPARE(rec.count(), 1);
        QCOMPARE(rec.at(0).at(0).toLongLong(), a);
        QCOMPARE(fake->loads, 1);
        QCOMPARE(fake->source(), dir.filePath("meeting.m4a"));
        QCOMPARE(fake->seeks.last(), qint64(8000));
        QVERIFY(pc->isPlaying());
        QCOMPARE(pc->position(), qint64(8000));
        QCOMPARE(pc->duration(), qint64(20000));
        QVERIFY(state.count() >= 1);
        QCOMPARE(state.last().at(0).value<State>(), State::Playing);
    }
    void playingTheSameRecordingOnlySeeksItNeverReloads()
    {
        QVERIFY(pc->play(a, 0));
        QVERIFY(pc->play(a, 12000));
        QVERIFY(pc->play(a, 4000));
        QCOMPARE(fake->loads, 1);
        QCOMPARE(fake->seeks.last(), qint64(4000));
        QCOMPARE(pc->position(), qint64(4000));
        QVERIFY(pc->isPlaying());
    }
    void switchingRecordingsStopsTheOldOne()
    {
        QSignalSpy rec(pc.get(), &PlaybackController::recordingChanged);
        QVERIFY(pc->play(a, 0));
        QVERIFY(pc->play(b, 0));
        QCOMPARE(pc->recordingId(), b);
        QCOMPARE(fake->loads, 2);
        QVERIFY(fake->stops >= 1);
        QCOMPARE(rec.count(), 2);
        QCOMPARE(pc->title(), QString("memo.wav"));
    }
    void toggleAndPauseBehaveLikeAPlayer()
    {
        pc->toggle();                                                      // nothing loaded: nothing happens
        QCOMPARE(fake->plays, 0);
        QVERIFY(pc->play(a, 0));
        pc->toggle();
        QCOMPARE(pc->state(), State::Paused);
        pc->toggle();
        QCOMPARE(pc->state(), State::Playing);
        pc->pause();
        QCOMPARE(pc->state(), State::Paused);
        pc->stop();                                                        // unload
        QCOMPARE(pc->recordingId(), qint64(0));
        QVERIFY(pc->title().isEmpty());
        QVERIFY(pc->sentence().isEmpty());
        pc->toggle();                                                      // still nothing
        QCOMPARE(pc->state(), State::Stopped);
    }
    void afterTheEndPlayStartsFromTheTopAndTheRecordingStaysLoaded()
    {
        QVERIFY(pc->play(a, 15000));
        fake->advance(10000);                                             // past the end (20 s)
        QCOMPARE(pc->state(), State::Stopped);
        QCOMPARE(pc->recordingId(), a);                                    // still the current one: the ▶ replays it
        fake->seeks.clear();
        pc->toggle();
        QCOMPARE(pc->state(), State::Playing);
        QCOMPARE(fake->seeks.last(), qint64(0));
        QCOMPARE(fake->loads, 1);
    }
    void seekAndSkipAreClampedToTheDuration()
    {
        QVERIFY(pc->play(a, 10000));
        pc->skip(PlaybackController::kSkipMs);
        QCOMPARE(pc->position(), qint64(15000));
        pc->skip(-PlaybackController::kSkipMs);
        QCOMPARE(pc->position(), qint64(10000));
        pc->skip(-60000);
        QCOMPARE(pc->position(), qint64(0));
        pc->seekTo(999999);
        QCOMPARE(pc->position(), qint64(20000));
        pc->seekTo(-5);
        QCOMPARE(pc->position(), qint64(0));
        pc->stop();
        fake->seeks.clear();
        pc->seekTo(5000);                                                  // nothing loaded: ignored
        QVERIFY(fake->seeks.isEmpty());
    }
    void followsTheSentenceBeingSpoken()
    {
        QSignalSpy sentence(pc.get(), &PlaybackController::sentenceChanged);
        QVERIFY(pc->play(a, 0));
        QCOMPARE(pc->sentence(), QString("最初の文です。"));
        fake->advance(1000);                                              // still inside the first one: no new signal
        const int n = sentence.count();
        fake->advance(3000);                                              // 4 s -> second sentence
        QCOMPARE(pc->sentence(), QString("二番目の文です。"));
        QCOMPARE(sentence.count(), n + 1);
        QCOMPARE(sentence.last().at(0).toString(), QString("二番目の文です。"));
        QCOMPARE(sentence.last().at(1).toLongLong(), qint64(4000));
        pc->seekTo(13000);                                                 // a seek updates it at once
        QCOMPARE(pc->sentence(), QString("四番目の文です。"));
        pc->seekTo(0);
        QCOMPARE(pc->sentence(), QString("最初の文です。"));
        QVERIFY(pc->play(b, 0));
        QCOMPARE(pc->sentence(), QString("メモの文です。"));
    }
    void sentencesShowTheUsersCorrections()
    {
        const auto tok = createTokenizer();
        const auto rows = TranscriptRepository(db).loadSegments(a);
        QVERIFY(RecordingEditor(db, *tok).setSegmentText(a, rows[1].id, "直した二番目の文です。"));
        QVERIFY(pc->play(a, 4000));
        QCOMPARE(pc->sentence(), QString("直した二番目の文です。"));
    }
    void recordingsWithoutATranscriptStillPlay()
    {
        const qint64 noText = add("silent.wav", {});
        QVERIFY(pc->play(noText, 0));
        QVERIFY(pc->isPlaying());
        QVERIFY(pc->sentence().isEmpty());
        fake->advance(2000);
        QVERIFY(pc->sentence().isEmpty());
    }

    // ---- failures
    void aMissingFileIsReportedAndTheCurrentPlaybackIsLeftAlone()
    {
        QVERIFY(pc->play(a, 0));
        QSignalSpy err(pc.get(), &PlaybackController::errorOccurred);
        QVERIFY(!pc->play(missing, 0));
        QCOMPARE(err.count(), 1);
        QVERIFY2(err.at(0).at(0).toString().contains("gone.mp3"), qPrintable(err.at(0).at(0).toString()));
        QCOMPARE(pc->recordingId(), a);                                    // still playing the previous one
        QVERIFY(pc->isPlaying());
        QCOMPARE(fake->loads, 1);
        QVERIFY(!pc->play(9999, 0));
        QCOMPARE(err.count(), 2);
    }
    void aFileThatCannotBeDecodedReportsAndReloadsOnTheNextTry()
    {
        QSignalSpy err(pc.get(), &PlaybackController::errorOccurred);
        QVERIFY(pc->play(corrupt, 0));                                     // exists: loading is attempted ...
        QTRY_COMPARE(err.count(), 1);                                      // ... and the backend reports the failure
        QVERIFY(err.at(0).at(0).toString().contains("ストリーム"));
        QCOMPARE(pc->recordingId(), corrupt);                              // shown in the mini player with the error
        const int loads = fake->loads;
        QVERIFY(pc->play(corrupt, 0));                                     // retry reloads instead of "just seeking"
        QCOMPARE(fake->loads, loads + 1);
    }
    void rateAndVolumeReachThePlayer()
    {
        pc->setRate(1.5);
        pc->setVolume(0.4);
        QCOMPARE(fake->rate(), 1.5);
        QCOMPARE(pc->volume(), 0.4);
        QCOMPARE(pc->rate(), 1.5);
    }
};

QTEST_MAIN(TstPlayback)
#include "tst_playback.moc"
