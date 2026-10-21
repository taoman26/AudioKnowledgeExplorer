#include <QFile>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include "core/Database.h"
#include "core/FFmpegDecoder.h"
#include "core/TranscriptRepository.h"
#include "core/WhisperTranscriber.h"

using namespace ake;

namespace {
// Model/sample locations come from the environment so the suite also runs (skipping) on machines without them.
//   AKE_TEST_MODEL  a multilingual ggml model, e.g. ggml-tiny.bin
//   AKE_TEST_JFK    whisper.cpp's samples/jfk.wav (public domain, English)
QString envPath(const char* name, const QString& fallback = QString())
{
    const QString v = qEnvironmentVariable(name);
    const QString p = v.isEmpty() ? fallback : v;
    return QFile::exists(p) ? p : QString();
}
QString modelPath()
{
    return envPath("AKE_TEST_MODEL");
}
QString jfkPath()
{
    return envPath("AKE_TEST_JFK");
}
} // namespace

class TstTranscribe : public QObject {
    Q_OBJECT
    Database db;
    qint64 recId = 0;

    TranscriptResult sample(const QString& lang, const QStringList& texts)
    {
        TranscriptResult r;
        r.language = lang;
        r.model = "ggml-test";
        qint64 t = 0;
        for (const QString& s : texts) { r.segments.append({ t, t + 1500, s }); t += 1500; }
        return r;
    }

private slots:
    void init()
    {
        QVERIFY(db.open(":memory:"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("INSERT INTO folders(id,path) VALUES(1,'/f')"));
        QVERIFY(q.exec("INSERT INTO recordings(id,folder_id,rel_path,abs_path,created_at,updated_at) VALUES(1,1,'a.mp3','/f/a.mp3',0,0)"));
        recId = 1;
    }

    // ---- repository (no model needed)
    void repositorySaveAndLoadRoundTrip()
    {
        TranscriptRepository repo(db);
        QString err;
        const qint64 tid = repo.save(recId, sample("ja", { "今日はDockerの話です。", "Podmanへ移行します。" }), &err);
        QVERIFY2(tid > 0, qPrintable(err));
        TranscriptResult out;
        QVERIFY(repo.loadCurrent(recId, &out));
        QCOMPARE(out.language, QString("ja"));
        QCOMPARE(out.model, QString("ggml-test"));
        QCOMPARE(out.segments.size(), 2);
        QCOMPARE(out.segments[1].text, QString("Podmanへ移行します。"));
        QCOMPARE(out.segments[1].startMs, qint64(1500));
        QCOMPARE(out.segments[1].endMs, qint64(3000));
    }
    void decodeSettingsAreStoredWithTheTranscript()
    {
        TranscriptRepository repo(db);
        TranscriptResult r = sample("ja", { "text" });
        r.settings = "beam=5 threads=3 lang=auto";
        QVERIFY(repo.save(recId, r) > 0);
        TranscriptResult back;
        QVERIFY(repo.loadCurrent(recId, &back));
        QCOMPARE(back.settings, QString("beam=5 threads=3 lang=auto"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("SELECT model_version FROM transcripts WHERE is_current=1"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QString("beam=5 threads=3 lang=auto"));
    }
    void emptyLanguageAndModelAreStorable() // regression: null QString bound as NULL violated NOT NULL columns
    {
        TranscriptRepository repo(db);
        TranscriptResult r;               // language/model left null
        r.segments.append({ 0, 1000, "text" });
        QString err;
        QVERIFY2(repo.save(recId, r, &err) > 0, qPrintable(err));
        TranscriptResult back;
        QVERIFY(repo.loadCurrent(recId, &back));
        QCOMPARE(back.segments.size(), 1);
    }
    void newGenerationReplacesCurrentButKeepsHistory()
    {
        TranscriptRepository repo(db);
        QVERIFY(repo.save(recId, sample("ja", { "古い" })) > 0);
        QVERIFY(repo.save(recId, sample("ja", { "新しい", "二つ目" })) > 0);
        QCOMPARE(repo.generationCount(recId), 2);
        TranscriptResult out;
        QVERIFY(repo.loadCurrent(recId, &out));
        QCOMPARE(out.segments.size(), 2);
        QCOMPARE(out.segments[0].text, QString("新しい"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("SELECT count(*) FROM transcripts WHERE is_current=1"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1);
    }
    void userEditedTextWinsOnLoad()
    {
        TranscriptRepository repo(db);
        QVERIFY(repo.save(recId, sample("ja", { "誤認識です" })) > 0);
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("UPDATE segments SET edited_text='修正後です'"));
        TranscriptResult out;
        QVERIFY(repo.loadCurrent(recId, &out));
        QCOMPARE(out.segments[0].text, QString("修正後です"));
    }
    void saveIsAtomicAndRejectsUnknownRecording()
    {
        TranscriptRepository repo(db);
        QString err;
        QCOMPARE(repo.save(999, sample("ja", { "x" }), &err), qint64(0));
        QVERIFY(!err.isEmpty());
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("SELECT (SELECT count(*) FROM transcripts), (SELECT count(*) FROM segments)"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 0);
        QCOMPARE(q.value(1).toInt(), 0);
        TranscriptResult out;
        QVERIFY(!repo.loadCurrent(recId, &out)); // nothing saved yet
    }
    void deletingRecordingRemovesItsTranscripts()
    {
        TranscriptRepository repo(db);
        QVERIFY(repo.save(recId, sample("ja", { "a", "b" })) > 0);
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("DELETE FROM recordings WHERE id=1"));
        QVERIFY(q.exec("SELECT (SELECT count(*) FROM transcripts), (SELECT count(*) FROM segments)"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt() + q.value(1).toInt(), 0);
    }

    // ---- whisper (model needed for some)
    void loadErrors()
    {
        WhisperTranscriber w;
        QString err;
        QVERIFY(!w.load("/no/such/model.bin", &err));
        QVERIFY2(err.contains("not found"), qPrintable(err));
        QVERIFY(!w.isLoaded());

        QTemporaryDir dir;
        QFile junk(dir.filePath("junk.bin"));
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write(QByteArray(4096, 'x'));
        junk.close();
        QVERIFY(!w.load(junk.fileName(), &err));
        QVERIFY2(err.contains("cannot load model"), qPrintable(err));

        TranscriptResult r;
        QVERIFY(!w.transcribe(std::vector<float>(16000, 0.f), {}, &r, &err));
        QCOMPARE(err, QString("no model loaded"));
    }
    void silenceTranscribesWithoutCrashing()
    {
        const QString m = modelPath();
        if (m.isEmpty()) QSKIP("set AKE_TEST_MODEL to a ggml model");
        WhisperTranscriber w;
        QString err;
        QVERIFY2(w.load(m, &err), qPrintable(err));
        QCOMPARE(w.modelName(), QString("ggml-tiny"));
        TranscriptResult r;
        QVERIFY2(w.transcribe(std::vector<float>(16000 * 3, 0.f), {}, &r, &err), qPrintable(err));
        QVERIFY(!r.language.isEmpty());
        for (const auto& s : r.segments) { QVERIFY(s.endMs >= s.startMs); QVERIFY(!s.text.isEmpty()); }
        QVERIFY(!w.transcribe({}, {}, &r, &err));
        QCOMPARE(err, QString("no audio"));
    }
    void englishSpeechIsRecognizedWithTimestamps()
    {
        const QString m = modelPath(), jfk = jfkPath();
        if (m.isEmpty() || jfk.isEmpty()) QSKIP("set AKE_TEST_MODEL and AKE_TEST_JFK");
        FFmpegDecoder dec;
        std::vector<float> pcm; AudioInfo info; QString err;
        QVERIFY2(dec.decode(jfk, &pcm, &info, &err), qPrintable(err));
        WhisperTranscriber w;
        QVERIFY2(w.load(m, &err), qPrintable(err));
        TranscriptResult r;
        QList<double> progress;
        QVERIFY2(w.transcribe(pcm, {}, &r, &err, [&](double f) { progress << f; return true; }), qPrintable(err));
        QCOMPARE(r.language, QString("en")); // auto-detected
        QVERIFY2(r.settings.contains("greedy") && r.settings.contains("lang=auto") && r.settings.contains("threads="), qPrintable(r.settings));
        QVERIFY(!r.segments.isEmpty());
        QString all;
        for (const auto& s : r.segments) all += s.text + " ";
        QVERIFY2(all.toLower().contains("country"), qPrintable(all));
        QVERIFY2(all.toLower().contains("ask not"), qPrintable(all));
        // timestamps are monotonic and inside the clip
        qint64 prevEnd = 0;
        for (const auto& s : r.segments) {
            QVERIFY(s.startMs >= prevEnd - 1);
            QVERIFY(s.endMs > s.startMs);
            QVERIFY(s.endMs <= info.durationMs + 1000);
            prevEnd = s.endMs;
        }
        QVERIFY(!progress.isEmpty());
        QCOMPARE(progress.last(), 1.0);
    }
    void cancellationStopsTranscription()
    {
        const QString m = modelPath(), jfk = jfkPath();
        if (m.isEmpty() || jfk.isEmpty()) QSKIP("set AKE_TEST_MODEL and AKE_TEST_JFK");
        FFmpegDecoder dec;
        std::vector<float> pcm; QString err;
        QVERIFY(dec.decode(jfk, &pcm, nullptr, &err));
        std::vector<float> longPcm;
        for (int i = 0; i < 8; ++i) longPcm.insert(longPcm.end(), pcm.begin(), pcm.end()); // ~90 s
        WhisperTranscriber w;
        QVERIFY(w.load(m, &err));
        TranscriptResult r;
        QElapsedTimer t; t.start();
        QVERIFY(!w.transcribe(longPcm, {}, &r, &err, [](double) { return false; }));
        QCOMPARE(err, QString("cancelled"));
        QVERIFY2(t.elapsed() < 40000, "cancel must not wait for the whole file");   // one 30 s window takes ~20 s in an ASan build; the whole ~90 s would take 60+
        // the instance stays usable after a cancel
        QVERIFY2(w.transcribe(pcm, {}, &r, &err), qPrintable(err));
        QVERIFY(!r.segments.isEmpty());
    }
    void transcriptCanBeStoredAndReloaded()
    {
        const QString m = modelPath(), jfk = jfkPath();
        if (m.isEmpty() || jfk.isEmpty()) QSKIP("set AKE_TEST_MODEL and AKE_TEST_JFK");
        FFmpegDecoder dec;
        std::vector<float> pcm; QString err;
        QVERIFY(dec.decode(jfk, &pcm, nullptr, &err));
        WhisperTranscriber w;
        QVERIFY(w.load(m, &err));
        TranscriptResult r;
        QVERIFY(w.transcribe(pcm, { "en", 0 }, &r, &err));
        TranscriptRepository repo(db);
        QVERIFY2(repo.save(recId, r, &err) > 0, qPrintable(err));
        TranscriptResult back;
        QVERIFY(repo.loadCurrent(recId, &back));
        QCOMPARE(back.segments.size(), r.segments.size());
        QCOMPARE(back.segments.first().text, r.segments.first().text);
        QCOMPARE(back.model, QString("ggml-tiny"));
    }
};

QTEST_MAIN(TstTranscribe)
#include "tst_transcribe.moc"
