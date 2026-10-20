#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtMath>
#include <QtTest>

#include "core/FFmpegDecoder.h"

using namespace ake;

namespace {

// 16-bit PCM WAV written by hand so the baseline test does not depend on the ffmpeg binary.
void writeWav(const QString& path, int rate, int channels, double seconds, double hz, double amp = 0.5)
{
    const int frames = int(rate * seconds);
    QByteArray data;
    QDataStream ds(&data, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    for (int i = 0; i < frames; ++i) {
        const qint16 v = qint16(amp * 32767 * qSin(2 * M_PI * hz * i / rate));
        for (int c = 0; c < channels; ++c) ds << v;
    }
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QDataStream o(&f);
    o.setByteOrder(QDataStream::LittleEndian);
    f.write("RIFF"); o << quint32(36 + data.size()); f.write("WAVEfmt ");
    o << quint32(16) << quint16(1) << quint16(channels) << quint32(rate) << quint32(rate * channels * 2)
      << quint16(channels * 2) << quint16(16);
    f.write("data"); o << quint32(data.size());
    f.write(data);
}

double peak(const std::vector<float>& v)
{
    float m = 0;
    for (float x : v) m = qMax(m, qAbs(x));
    return m;
}
// Frequency estimate from zero crossings (pure sine, so this is robust).
double frequencyOf(const std::vector<float>& v)
{
    int cross = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if ((v[i - 1] < 0) != (v[i] < 0)) cross++;
    return cross / 2.0 / (double(v.size()) / kDecodeSampleRate);
}
bool haveFfmpegCli() { return !QStandardPaths::findExecutable("ffmpeg").isEmpty(); }
bool makeWithFfmpeg(const QString& out, double hz, double sec, const QStringList& extra)
{
    QStringList args = { "-v", "error", "-y", "-f", "lavfi", "-i", QString("sine=frequency=%1:duration=%2").arg(hz).arg(sec) };
    args += extra;
    args << out;
    return QProcess::execute("ffmpeg", args) == 0;
}
} // namespace

class TstDecoder : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    FFmpegDecoder dec;
    QString p(const QString& n) const { return dir.filePath(n); }

private slots:
    void wavStereo44kDecodesToMono16k()
    {
        writeWav(p("a.wav"), 44100, 2, 2.0, 440);
        std::vector<float> pcm; AudioInfo info; QString err;
        QVERIFY2(dec.decode(p("a.wav"), &pcm, &info, &err), qPrintable(err));
        QVERIFY2(qAbs(int(pcm.size()) - 32000) <= 64, qPrintable(QString::number(pcm.size())));
        QCOMPARE(info.sampleRate, 44100);
        QCOMPARE(info.channels, 2);
        QVERIFY(info.codec.startsWith("pcm_s16le"));
        QVERIFY(qAbs(info.durationMs - 2000) <= 10);
        // identical L/R at 0.5: the downmix must not get louder than the source (no +3 dB, no clipping)
        QVERIFY2(qAbs(peak(pcm) - 0.5) < 0.03, qPrintable(QString::number(peak(pcm))));
        QVERIFY2(qAbs(frequencyOf(pcm) - 440) < 8, qPrintable(QString::number(frequencyOf(pcm))));
    }
    void fullScaleStereoDoesNotClip()
    {
        writeWav(p("loud.wav"), 16000, 2, 1.0, 440, 0.99);
        std::vector<float> pcm; QString err;
        QVERIFY2(dec.decode(p("loud.wav"), &pcm, nullptr, &err), qPrintable(err));
        QVERIFY2(peak(pcm) <= 1.0f, qPrintable(QString::number(peak(pcm))));
    }
    void probeIsConsistentWithDecode()
    {
        writeWav(p("b.wav"), 8000, 1, 1.5, 300);
        AudioInfo pi; QString err;
        QVERIFY2(dec.probe(p("b.wav"), &pi, &err), qPrintable(err));
        QCOMPARE(pi.sampleRate, 8000);
        QCOMPARE(pi.channels, 1);
        QVERIFY(qAbs(pi.durationMs - 1500) <= 10);
        std::vector<float> pcm;
        QVERIFY(dec.decode(p("b.wav"), &pcm, nullptr, &err));
        QVERIFY(qAbs(pcm.size() - 24000) <= 64);
        QVERIFY2(qAbs(frequencyOf(pcm) - 300) < 6, "upsampling 8k->16k must keep the pitch");
    }
    void allSupportedFormatsDecodeWithCorrectPitchAndLength()
    {
        if (!haveFfmpegCli()) QSKIP("ffmpeg CLI not available to create fixtures");
        struct Case { const char* file; QStringList args; double hz; };
        const QList<Case> cases = {
            { "t.mp3",  { "-c:a", "libmp3lame", "-ar", "44100", "-ac", "2" }, 523 },
            { "t.m4a",  { "-c:a", "aac", "-ar", "48000", "-ac", "2" },        440 },
            { "t.ogg",  { "-c:a", "libvorbis", "-ar", "22050", "-ac", "1" },  600 },
            { "t.flac", { "-c:a", "flac", "-ar", "48000", "-ac", "2" },       700 },
            { "t.wav",  { "-c:a", "pcm_s16le", "-ar", "8000", "-ac", "1" },   350 },
        };
        for (const Case& c : cases) {
            QVERIFY2(makeWithFfmpeg(p(c.file), c.hz, 2.0, c.args), c.file);
            std::vector<float> pcm; AudioInfo info; QString err;
            QVERIFY2(dec.decode(p(c.file), &pcm, &info, &err), qPrintable(QString(c.file) + ": " + err));
            const double sec = double(pcm.size()) / kDecodeSampleRate;
            QVERIFY2(qAbs(sec - 2.0) < 0.12, qPrintable(QString("%1 length %2s").arg(c.file).arg(sec)));
            QVERIFY2(qAbs(frequencyOf(pcm) - c.hz) < c.hz * 0.03,
                     qPrintable(QString("%1 freq %2 want %3").arg(c.file).arg(frequencyOf(pcm)).arg(c.hz)));
            // ffmpeg's `sine` source is mono with amplitude 1/8; encoding it to stereo upmixes at -3 dB (~0.088 per
            // channel); lossy codecs and resampling add a little overshoot. Silence or clipping would be far outside.
            QVERIFY2(peak(pcm) > 0.07 && peak(pcm) < 0.16, qPrintable(QString("%1 peak %2").arg(c.file).arg(peak(pcm))));
            QVERIFY2(qAbs(info.durationMs - 2000) < 120, qPrintable(QString("%1 dur %2").arg(c.file).arg(info.durationMs)));
            AudioInfo pi;
            QVERIFY2(dec.probe(p(c.file), &pi, &err), c.file);
            QVERIFY2(qAbs(pi.durationMs - 2000) < 150, qPrintable(QString("%1 probe %2").arg(c.file).arg(pi.durationMs)));
        }
    }
    void videoContainersGiveTheirSoundNotThePicture()
    {
        if (!haveFfmpegCli()) QSKIP("ffmpeg CLI not available to create fixtures");
        struct Case { const char* file; QStringList codecs; double hz; };
        const QList<Case> cases = {
            { "v.mp4",  { "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac" }, 440 },
            { "v.mov",  { "-c:v", "mpeg4", "-c:a", "aac" },                          520 },
            { "v.mkv",  { "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "flac" }, 610 },
            { "v.webm", { "-c:v", "libvpx", "-c:a", "libopus", "-ar", "48000" },     700 },
        };
        int made = 0;
        for (const Case& c : cases) {
            // the video stream comes first: the decoder has to pick the audio one
            QStringList args = { "-v", "error", "-y", "-f", "lavfi", "-i", "color=c=blue:s=160x120:d=2", "-f", "lavfi", "-i",
                                 QString("sine=frequency=%1:duration=2").arg(c.hz) };
            args += c.codecs;
            args << "-shortest" << p(c.file);
            if (QProcess::execute("ffmpeg", args) != 0) { qWarning("no encoder for %s in this ffmpeg: skipped", c.file); continue; }   // e.g. Haiku's ffmpeg has no libx264
            ++made;
            std::vector<float> pcm; AudioInfo info; QString err;
            QVERIFY2(dec.decode(p(c.file), &pcm, &info, &err), qPrintable(QString(c.file) + ": " + err));
            const double sec = double(pcm.size()) / kDecodeSampleRate;
            QVERIFY2(qAbs(sec - 2.0) < 0.15, qPrintable(QString("%1 length %2s").arg(c.file).arg(sec)));
            QVERIFY2(qAbs(frequencyOf(pcm) - c.hz) < c.hz * 0.03, qPrintable(QString("%1 freq %2").arg(c.file).arg(frequencyOf(pcm))));
            AudioInfo pi;
            QVERIFY2(dec.probe(p(c.file), &pi, &err), c.file);
            QVERIFY2(qAbs(pi.durationMs - 2000) < 200, qPrintable(QString("%1 probe %2").arg(c.file).arg(pi.durationMs)));
        }
        // a video without any sound is reported, not silently registered as an empty transcript
        QStringList mute = { "-v", "error", "-y", "-f", "lavfi", "-i", "color=c=blue:s=160x120:d=1", "-c:v", "libx264", "-pix_fmt", "yuv420p", p("mute.mp4") };
        if (QProcess::execute("ffmpeg", mute) != 0) {
            if (!made) QSKIP("this ffmpeg cannot encode any of the video fixtures");
            return;
        }
        if (!made) QSKIP("this ffmpeg cannot encode any of the video fixtures");
        std::vector<float> pcm; QString err;
        QVERIFY(!dec.decode(p("mute.mp4"), &pcm, nullptr, &err));
        QVERIFY2(!err.isEmpty(), "an error message is expected");
    }
    void readsContainerMetadata()
    {
        if (!haveFfmpegCli()) QSKIP("ffmpeg CLI not available to create fixtures");
        QVERIFY(makeWithFfmpeg(p("m.m4a"), 440, 1.0,
                               { "-c:a", "aac", "-metadata", "title=Docker移行会議", "-metadata", "creation_time=2026-03-01T10:20:30Z" }));
        AudioInfo info; QString err;
        QVERIFY2(dec.probe(p("m.m4a"), &info, &err), qPrintable(err));
        QCOMPARE(info.title, QString("Docker移行会議"));
        QVERIFY(info.creationTime.isValid());
        QCOMPARE(info.creationTime.toUTC(), QDateTime(QDate(2026, 3, 1), QTime(10, 20, 30), Qt::UTC));
        // no tags -> empty title, invalid time
        writeWav(p("n.wav"), 16000, 1, 0.5, 440);
        QVERIFY(dec.probe(p("n.wav"), &info, &err));
        QVERIFY(info.title.isEmpty());
        QVERIFY(!info.creationTime.isValid());
    }
    void reportsErrorsForBadInput()
    {
        std::vector<float> pcm; AudioInfo info; QString err;
        QVERIFY(!dec.decode(p("missing.mp3"), &pcm, &info, &err));
        // OS wording is locale dependent: only check the prefix and that it is not the generic FFmpeg text
        QVERIFY2(err.startsWith("cannot open: ") && !err.contains("Error number"), qPrintable(err));
        QVERIFY(pcm.empty());

        QFile g(p("garbage.mp3"));
        QVERIFY(g.open(QIODevice::WriteOnly));
        g.write(QByteArray(5000, '\x7f'));
        g.close();
        err.clear();
        QVERIFY(!dec.decode(p("garbage.mp3"), &pcm, &info, &err));
        QVERIFY2(!err.isEmpty(), "garbage must produce an error message");
        QVERIFY(!dec.probe(p("garbage.mp3"), &info, &err));

        QFile e(p("empty.wav"));
        QVERIFY(e.open(QIODevice::WriteOnly));
        e.close();
        QVERIFY(!dec.decode(p("empty.wav"), &pcm, &info, &err));
    }
    void truncatedFileDecodesWhatIsThere()
    {
        writeWav(p("full.wav"), 16000, 1, 2.0, 440);
        QFile f(p("full.wav"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray half = f.read(f.size() / 2);
        f.close();
        QFile t(p("cut.wav"));
        QVERIFY(t.open(QIODevice::WriteOnly));
        t.write(half);
        t.close();
        std::vector<float> pcm; QString err;
        QVERIFY2(dec.decode(p("cut.wav"), &pcm, nullptr, &err), qPrintable(err));
        QVERIFY2(pcm.size() > 12000 && pcm.size() < 20000, qPrintable(QString::number(pcm.size())));
    }
    void progressIsMonotonicAndEndsAtOne()
    {
        writeWav(p("long.wav"), 16000, 1, 20.0, 440);
        std::vector<float> pcm; QString err;
        QList<double> seen;
        QVERIFY2(dec.decode(p("long.wav"), &pcm, nullptr, &err, [&](double f) { seen << f; return true; }), qPrintable(err));
        QVERIFY(seen.size() > 3);
        for (int i = 1; i < seen.size(); ++i) QVERIFY(seen[i] >= seen[i - 1]);
        QCOMPARE(seen.last(), 1.0);
        QVERIFY(seen.first() >= 0 && seen.first() < 0.5);
    }
    void cancelStopsEarly()
    {
        writeWav(p("long2.wav"), 16000, 1, 60.0, 440);
        std::vector<float> pcm; QString err;
        int calls = 0;
        QVERIFY(!dec.decode(p("long2.wav"), &pcm, nullptr, &err, [&](double) { return ++calls < 3; }));
        QCOMPARE(err, QString("cancelled"));
        QCOMPARE(calls, 3);
        QVERIFY(pcm.size() < 16000u * 60u / 2); // stopped long before the end
    }
    void concurrentDecodesAreIndependent()
    {
        writeWav(p("c1.wav"), 16000, 1, 3.0, 300);
        writeWav(p("c2.wav"), 22050, 2, 3.0, 800);
        double f1 = 0, f2 = 0;
        QThread* t1 = QThread::create([&] { FFmpegDecoder d; std::vector<float> v; QString e;
                                            for (int i = 0; i < 5; ++i) d.decode(p("c1.wav"), &v, nullptr, &e); f1 = frequencyOf(v); });
        QThread* t2 = QThread::create([&] { FFmpegDecoder d; std::vector<float> v; QString e;
                                            for (int i = 0; i < 5; ++i) d.decode(p("c2.wav"), &v, nullptr, &e); f2 = frequencyOf(v); });
        t1->start(); t2->start(); t1->wait(); t2->wait();
        delete t1; delete t2;
        QVERIFY(qAbs(f1 - 300) < 6);
        QVERIFY(qAbs(f2 - 800) < 16);
    }
    void unicodePathWorks()
    {
        writeWav(p("会議 録音 #1.wav"), 16000, 1, 0.5, 440);
        std::vector<float> pcm; QString err;
        QVERIFY2(dec.decode(p("会議 録音 #1.wav"), &pcm, nullptr, &err), qPrintable(err));
        QVERIFY(pcm.size() > 7000);
    }
};

QTEST_MAIN(TstDecoder)
#include "tst_decoder.moc"
