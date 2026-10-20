#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtMath>
#include <QtTest>

#include "platform/QtAudioPlayer.h"

using namespace ake;
using State = IAudioPlayer::State;

// Exercises the real QtMultimedia backend with real files (volume 0: nothing is audible). Needs a working media
// backend; where playback cannot start at all (no GStreamer plugins) the tests skip instead of failing.
class TstQtPlayer : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    QString wav;

    static void writeWav(const QString& path, double seconds)
    {
        const int rate = 16000, n = int(rate * seconds);
        QByteArray data;
        QDataStream ds(&data, QIODevice::WriteOnly);
        ds.setByteOrder(QDataStream::LittleEndian);
        for (int i = 0; i < n; ++i) ds << qint16(0.3 * 32767 * qSin(2 * M_PI * 440 * i / rate));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QDataStream o(&f);
        o.setByteOrder(QDataStream::LittleEndian);
        f.write("RIFF"); o << quint32(36 + data.size()); f.write("WAVEfmt ");
        o << quint32(16) << quint16(1) << quint16(1) << quint32(rate) << quint32(rate * 2) << quint16(2) << quint16(16);
        f.write("data"); o << quint32(data.size());
        f.write(data);
    }
    static bool makeWithFfmpeg(const QString& out, double sec, const QStringList& codec)
    {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) return false;
        QStringList args = { "-v", "error", "-y", "-f", "lavfi", "-i", QString("sine=frequency=440:duration=%1").arg(sec) };
        args += codec;
        args << out;
        return QProcess::execute("ffmpeg", args) == 0;
    }

    // true when the backend is usable on this machine
    bool playsAtAll(const QString& file)
    {
        QtAudioPlayer p;
        p.setVolume(0);
        QSignalSpy err(&p, &IAudioPlayer::errorOccurred);
        p.load(file);
        p.play();
        QTest::qWait(1200);
        return err.isEmpty() && p.state() == State::Playing;
    }

private slots:
    void initTestCase()
    {
        wav = dir.filePath("tone.wav");
        writeWav(wav, 2.0);
        if (!playsAtAll(wav)) QSKIP("QtMultimedia cannot play on this machine (no media backend?)");
    }

    void everySupportedFormatPlaysWithAKnownDuration()
    {
        struct Case { const char* file; QStringList codec; };
        const QList<Case> cases = { { "t.wav", {} }, { "t.mp3", { "-c:a", "libmp3lame" } }, { "t.m4a", { "-c:a", "aac" } },
                                    { "t.ogg", { "-c:a", "libvorbis" } }, { "t.flac", { "-c:a", "flac" } } };
        int tested = 0;
        for (const Case& c : cases) {
            const QString f = dir.filePath(c.file);
            if (QString(c.file) == "t.wav") writeWav(f, 2.0);
            else if (!makeWithFfmpeg(f, 2.0, c.codec)) continue;
            QtAudioPlayer p;
            p.setVolume(0);
            QSignalSpy err(&p, &IAudioPlayer::errorOccurred), dur(&p, &IAudioPlayer::durationChanged);
            p.load(f);
            p.play();
            QTRY_VERIFY2(p.position() > 400, c.file);                         // time really advances
            QVERIFY2(err.isEmpty(), c.file);
            QCOMPARE(p.state(), State::Playing);
            QVERIFY2(qAbs(p.duration() - 2000) < 150, qPrintable(QString("%1 duration %2").arg(c.file).arg(p.duration())));
            QVERIFY(!dur.isEmpty());
            ++tested;
        }
        QVERIFY2(tested >= 1, "at least the generated WAV must have been tested");
        qInfo("played %d format(s)", tested);
    }
    void pauseHoldsThePositionAndSeekMoves()
    {
        QtAudioPlayer p;
        p.setVolume(0);
        writeWav(dir.filePath("long.wav"), 6.0);
        p.load(dir.filePath("long.wav"));
        p.play();
        QTRY_VERIFY(p.position() > 300);
        p.pause();
        QTRY_COMPARE(p.state(), State::Paused);
        const qint64 held = p.position();
        QTest::qWait(500);
        QVERIFY2(qAbs(p.position() - held) < 120, "paused: the position must not move");
        p.seek(4000);
        QTRY_VERIFY2(qAbs(p.position() - 4000) < 300, qPrintable(QString::number(p.position())));
        QCOMPARE(p.state(), State::Paused);                                   // seeking does not resume
        p.play();
        QTRY_VERIFY(p.position() > 4100);
        p.stop();
        QCOMPARE(p.state(), State::Stopped);
    }
    void aSeekRequestedBeforePlaybackStartsIsHonoured()
    {
        // regression: the GStreamer backend ignores setPosition() until the pipeline is seekable
        QtAudioPlayer p;
        p.setVolume(0);
        QSignalSpy ended(&p, &IAudioPlayer::mediaEnded);
        QElapsedTimer t; t.start();
        p.load(wav);                                                          // 2 s file
        p.seek(1500);
        p.play();
        QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 1, 3000);
        QVERIFY2(t.elapsed() < 1500, qPrintable(QString("took %1 ms: the seek to 1.5 s was ignored").arg(t.elapsed())));
    }
    void endOfMediaStopsAndRewinds()
    {
        QtAudioPlayer p;
        p.setVolume(0);
        QSignalSpy ended(&p, &IAudioPlayer::mediaEnded), state(&p, &IAudioPlayer::stateChanged);
        p.load(wav);
        p.play();
        QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 1, 4000);
        QCOMPARE(p.state(), State::Stopped);
        QCOMPARE(p.position(), qint64(0));
        // play again from the top, without loading again
        p.play();
        QTRY_VERIFY(p.position() > 300);
        QCOMPARE(p.state(), State::Playing);
    }
    void speedControlIsDeclaredUnsupportedAndNeverStallsPlayback()
    {
        // Qt 6.2's GStreamer backend cannot change speed reliably (measured: ignored before playback, stalls the
        // pipeline when set during playback), so it declares that and ignores setRate().
        QtAudioPlayer p;
        p.setVolume(0);
        QVERIFY(!p.supportsRate());
        QSignalSpy ended(&p, &IAudioPlayer::mediaEnded);
        p.setRate(2.0);
        QCOMPARE(p.rate(), 1.0);                                              // the request is ignored, not half-applied
        QElapsedTimer t; t.start();
        p.load(wav);
        p.play();
        QTest::qWait(500);
        p.setRate(0.75);                                                      // even during playback: harmless
        QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 1, 3500);                    // it still plays through to the end
        QVERIFY2(t.elapsed() > 1500 && t.elapsed() < 2800, qPrintable(QString("2 s file took %1 ms").arg(t.elapsed())));
        p.setVolume(5); QCOMPARE(p.volume(), 1.0);
        p.setVolume(-1); QCOMPARE(p.volume(), 0.0);
    }
    void loadingAnotherFileWhilePlayingSwitchesOver()
    {
        QtAudioPlayer p;
        p.setVolume(0);
        writeWav(dir.filePath("other.wav"), 5.0);
        p.load(dir.filePath("long.wav"));
        p.play();
        QTRY_VERIFY(p.position() > 300);
        p.load(dir.filePath("other.wav"));
        QCOMPARE(p.source(), dir.filePath("other.wav"));
        p.play();
        QTRY_VERIFY2(qAbs(p.duration() - 5000) < 150, qPrintable(QString::number(p.duration())));
        QTRY_COMPARE(p.state(), State::Playing);
    }
    void aMissingFileReportsAnErrorEvenWhenTheListenerConnectsAfterLoad()
    {
        QtAudioPlayer p;
        p.load(dir.filePath("nope.mp3"));
        QSignalSpy err(&p, &IAudioPlayer::errorOccurred);                    // connected AFTER load(): the error is queued
        QTRY_COMPARE(err.count(), 1);
        QVERIFY2(err.at(0).at(0).toString().contains("nope.mp3"), qPrintable(err.at(0).at(0).toString()));
        QCOMPARE(p.state(), State::Stopped);
        p.play();                                                             // harmless
        QTest::qWait(100);
        QCOMPARE(p.state(), State::Stopped);
    }
    void aFileThatIsNotAudioReportsAnError()
    {
        QFile f(dir.filePath("garbage.mp3"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(4000, 'x'));
        f.close();
        QtAudioPlayer p;
        p.setVolume(0);
        QSignalSpy err(&p, &IAudioPlayer::errorOccurred);
        p.load(f.fileName());
        p.play();
        QTRY_VERIFY_WITH_TIMEOUT(!err.isEmpty(), 4000);
        QVERIFY(!err.at(0).at(0).toString().isEmpty());
        QVERIFY(p.state() != State::Playing);
        // the player is still usable afterwards
        p.load(wav);
        p.play();
        QTRY_VERIFY_WITH_TIMEOUT(p.position() > 300, 4000);
    }
    void unicodeAndSpacedPathsWork()
    {
        const QString path = dir.filePath("会議 録音 #1.wav");
        writeWav(path, 1.0);
        QtAudioPlayer p;
        p.setVolume(0);
        QSignalSpy err(&p, &IAudioPlayer::errorOccurred);
        p.load(path);
        p.play();
        QTRY_VERIFY(p.position() > 200);
        QVERIFY(err.isEmpty());
    }
};

QTEST_MAIN(TstQtPlayer)
#include "tst_qtplayer.moc"
