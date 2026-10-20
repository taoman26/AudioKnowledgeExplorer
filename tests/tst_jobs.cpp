#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QtTest>
#include <atomic>
#include <mutex>

#include "core/AnalysisPipeline.h"
#include "core/Database.h"
#include "core/ITokenizer.h"
#include "core/JobScheduler.h"
#include "core/TranscriptRepository.h"
#include "Fakes.h"

using namespace ake;

using namespace fakes;

class TstJobs : public QObject {
    Q_OBJECT
    Database db;
    FakeDecoder dec;

    qint64 addRecording(const QString& name, const QString& status = "new")
    {
        QSqlQuery q(db.handle());
        q.prepare("INSERT INTO recordings(folder_id,rel_path,abs_path,status,created_at,updated_at) VALUES(1,?,?,?,0,0)");
        q.addBindValue(name); q.addBindValue("/f/" + name); q.addBindValue(status);
        if (!q.exec()) qFatal("%s", qPrintable(q.lastError().text()));
        return q.lastInsertId().toLongLong();
    }
    QString field(qint64 id, const QString& col)
    {
        QSqlQuery q(db.handle());
        q.exec(QString("SELECT %1 FROM recordings WHERE id=%2").arg(col).arg(id));
        return q.next() ? q.value(0).toString() : QString();
    }
    QString jobField(qint64 id, const QString& col)
    {
        QSqlQuery q(db.handle());
        q.exec(QString("SELECT %1 FROM jobs WHERE recording_id=%2 ORDER BY id DESC LIMIT 1").arg(col).arg(id));
        return q.next() ? q.value(0).toString() : QString("<none>");
    }
    int transcriptCount(qint64 id) { return TranscriptRepository(db).generationCount(id); }

private slots:
    void init()
    {
        QVERIFY(db.open(":memory:"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("INSERT INTO folders(id,path) VALUES(1,'/f')"));
        g_running = 0; g_maxRunning = 0; g_stepMs = 5; g_factoryCalls = 0;
    }

    // ---- pipeline alone
    void pipelineReportsStagesInOrderAndMonotonicProgress()
    {
        AnalysisPipeline p(dec, factory());
        std::atomic<bool> cancel{ false };
        QStringList stages; QList<double> values;
        TranscriptResult r; AudioInfo info; QString err;
        QVERIFY2(p.run("/f/a.mp3", { "/m/small.bin", "ja", 0 }, cancel, [&](const QString& s, double v) {
            if (stages.isEmpty() || stages.last() != s) stages << s;
            values << v;
        }, &r, &info, &err), qPrintable(err));
        QCOMPARE(stages, QStringList({ "decode", "load", "transcribe" }));
        for (int i = 1; i < values.size(); ++i) QVERIFY2(values[i] >= values[i - 1], qPrintable(QString::number(i)));
        QCOMPARE(values.last(), 1.0);
        QCOMPARE(r.segments.size(), 2);
        QCOMPARE(info.durationMs, qint64(5000));
        QCOMPARE(g_lastModel, QString("/m/small.bin"));
        QCOMPARE(g_lastOptions.language, QString("ja"));
    }
    void pipelineReusesLoadedModelUntilItChanges()
    {
        AnalysisPipeline p(dec, factory());
        std::atomic<bool> cancel{ false };
        TranscriptResult r; AudioInfo i; QString e;
        QVERIFY(p.run("/f/a.mp3", { "/m/a.bin", "auto", 0 }, cancel, {}, &r, &i, &e));
        QVERIFY(p.run("/f/b.mp3", { "/m/a.bin", "auto", 0 }, cancel, {}, &r, &i, &e));
        QCOMPARE(g_factoryCalls.load(), 1);
        QVERIFY(p.run("/f/c.mp3", { "/m/b.bin", "auto", 0 }, cancel, {}, &r, &i, &e));
        QCOMPARE(g_factoryCalls.load(), 2);
        QCOMPARE(g_lastModel, QString("/m/b.bin"));
    }
    void pipelineErrorsAreReportedVerbatim()
    {
        AnalysisPipeline p(dec, factory());
        std::atomic<bool> cancel{ false };
        TranscriptResult r; AudioInfo i; QString e;
        QVERIFY(!p.run("/f/undecodable.mp3", { "/m/a.bin", "auto", 0 }, cancel, {}, &r, &i, &e));
        QCOMPARE(e, QString("cannot open: broken file"));
        QVERIFY(!p.run("/f/a.mp3", { "/m/missing.bin", "auto", 0 }, cancel, {}, &r, &i, &e));
        QVERIFY(e.startsWith("model file not found"));
        // after a failed load the next run tries again (no half-loaded state)
        QVERIFY(p.run("/f/a.mp3", { "/m/ok.bin", "auto", 0 }, cancel, {}, &r, &i, &e));
    }
    void pipelineCancelDuringDecodeAndDuringTranscribe()
    {
        AnalysisPipeline p(dec, factory());
        TranscriptResult r; AudioInfo i; QString e;
        std::atomic<bool> cancel{ false };
        QVERIFY(!p.run("/f/a.mp3", { "/m/a.bin", "auto", 0 }, cancel, [&](const QString& s, double) {
            if (s == "decode") cancel = true;
        }, &r, &i, &e));
        QCOMPARE(e, QString("cancelled"));
        cancel = false;
        int ticks = 0;
        QVERIFY(!p.run("/f/a.mp3", { "/m/a.bin", "auto", 0 }, cancel, [&](const QString& s, double) {
            if (s == "transcribe" && ++ticks == 3) cancel = true;
        }, &r, &i, &e));
        QCOMPARE(e, QString("cancelled"));
        QVERIFY(r.segments.isEmpty());
    }

    // ---- scheduler
    void jobRunsToDoneAndStoresResults()
    {
        JobScheduler s(db, dec, factory());
        s.setConfig({ "/m/small.bin", "auto", 0 });
        const qint64 id = addRecording("a.mp3");
        QList<QString> statuses;
        QObject::connect(&s, &JobScheduler::recordingChanged, [&](qint64 rid) { statuses << field(rid, "status"); });
        QList<double> progress;
        QObject::connect(&s, &JobScheduler::progressChanged, [&](qint64, const QString&, double v) { progress << v; });
        QSignalSpy idle(&s, &JobScheduler::idle);

        QVERIFY(s.enqueue(id));
        QCOMPARE(field(id, "status"), QString("transcribing")); // started right away (worker busy)
        QVERIFY(s.waitUntilIdle(5000));
        QCOMPARE(field(id, "status"), QString("done"));
        QCOMPARE(statuses.first(), QString("queued"));
        QVERIFY(statuses.contains("transcribing"));
        QCOMPARE(statuses.last(), QString("done"));
        QCOMPARE(idle.count(), 1);
        QCOMPARE(transcriptCount(id), 1);
        TranscriptResult back;
        QVERIFY(TranscriptRepository(db).loadCurrent(id, &back));
        QCOMPARE(back.segments.size(), 2);
        QCOMPARE(back.language, QString("ja")); // "auto" resolved by the (fake) engine
        QCOMPARE(field(id, "duration_ms"), QString("5000"));   // decoded length is stored
        QCOMPARE(field(id, "sample_rate"), QString("44100"));
        QCOMPARE(jobField(id, "state"), QString("done"));
        QCOMPARE(jobField(id, "progress").toDouble(), 1.0);
        QVERIFY(!progress.isEmpty());
        for (int i = 1; i < progress.size(); ++i) QVERIFY(progress[i] >= progress[i - 1]);
        QVERIFY(progress.last() >= 0.99);
    }
    void finishedJobIsIndexedForSearchWhenATokenizerIsSet()
    {
        const auto tokenizer = createTokenizer();
        JobScheduler s(db, dec, factory());
        s.setTokenizer(tokenizer.get());
        const qint64 id = addRecording("a.mp3");
        s.enqueue(id);
        QVERIFY(s.waitUntilIdle(5000));
        QCOMPARE(field(id, "status"), QString("done"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("SELECT count(*) FROM segments WHERE wakati<>''"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 2);                         // fake transcript: こんにちは / テストです
        QVERIFY(q.exec("SELECT count(*) FROM fts_segments WHERE fts_segments MATCH '\"" + tokenizer->wakati("テスト") + "\"'"));   // however the tokenizer splits it
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1);
        QVERIFY(q.exec("SELECT count(*) FROM fts_recording"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1);
        // the summary exists and is marked as machine-made
        QVERIFY(q.exec(QString("SELECT content, source FROM analyses WHERE recording_id=%1 AND kind='summary' AND is_current=1").arg(id)));
        QVERIFY(q.next());
        QVERIFY(q.value(0).toString().contains("テスト"));
        QCOMPARE(q.value(1).toString(), QString("ai-extractive"));
    }
    void jobsRunOneAtATimeInFifoOrder()
    {
        JobScheduler s(db, dec, factory());
        QList<qint64> done;
        QObject::connect(&s, &JobScheduler::recordingChanged, [&](qint64 rid) { if (field(rid, "status") == "done") done << rid; });
        QList<qint64> ids;
        for (int i = 0; i < 5; ++i) ids << addRecording(QString("r%1.mp3").arg(i));
        for (qint64 id : ids) QVERIFY(s.enqueue(id));
        QCOMPARE(s.queuedCount(), 4);
        QCOMPARE(s.runningId(), ids[0]);
        QVERIFY(s.waitUntilIdle(10000));
        QCOMPARE(done, ids);
        QCOMPARE(g_maxRunning.load(), 1);
    }
    void failureMarksErrorAndContinuesWithNextJob()
    {
        JobScheduler s(db, dec, factory());
        const qint64 bad = addRecording("undecodable.mp3"), good = addRecording("fine.mp3");
        s.enqueue(bad); s.enqueue(good);
        QVERIFY(s.waitUntilIdle(5000));
        QCOMPARE(field(bad, "status"), QString("error"));
        QCOMPARE(field(bad, "error_msg"), QString("cannot open: broken file"));
        QCOMPARE(jobField(bad, "state"), QString("failed"));
        QCOMPARE(jobField(bad, "attempts"), QString("1"));
        QCOMPARE(transcriptCount(bad), 0);
        QCOMPARE(field(good, "status"), QString("done"));
    }
    void missingModelGivesActionableError()
    {
        JobScheduler s(db, dec, factory());
        s.setConfig({ "/models/missing-model.bin", "auto", 0 });
        const qint64 id = addRecording("a.mp3");
        s.enqueue(id);
        QVERIFY(s.waitUntilIdle(5000));
        QCOMPARE(field(id, "status"), QString("error"));
        QVERIFY2(field(id, "error_msg").contains("/models/missing-model.bin"), qPrintable(field(id, "error_msg")));
    }
    void retryAfterErrorRunsAgainAndClearsError()
    {
        JobScheduler s(db, dec, factory());
        s.setConfig({ "/models/missing-model.bin", "auto", 0 });
        const qint64 id = addRecording("a.mp3");
        s.enqueue(id);
        QVERIFY(s.waitUntilIdle(5000));
        QCOMPARE(field(id, "status"), QString("error"));
        s.setConfig({ "/models/ok.bin", "auto", 0 }); // user fixed the model
        QVERIFY(s.enqueue(id));
        QCOMPARE(field(id, "error_msg"), QString(""));
        QVERIFY(s.waitUntilIdle(5000));
        QCOMPARE(field(id, "status"), QString("done"));
    }
    void cancelQueuedJobNeverRuns()
    {
        g_stepMs = 20;
        JobScheduler s(db, dec, factory());
        const qint64 a = addRecording("a.mp3"), b = addRecording("b.mp3");
        s.enqueue(a); s.enqueue(b);
        QVERIFY(s.cancel(b));
        QCOMPARE(field(b, "status"), QString("new"));
        QCOMPARE(s.queuedCount(), 0);
        QVERIFY(!s.cancel(b)); // no longer known
        QVERIFY(s.waitUntilIdle(10000));
        QCOMPARE(field(a, "status"), QString("done"));
        QCOMPARE(transcriptCount(b), 0);
        QCOMPARE(field(b, "status"), QString("new"));
    }
    void cancelRunningJobResetsToNewAndStartsNext()
    {
        g_stepMs = 30;
        JobScheduler s(db, dec, factory());
        const qint64 a = addRecording("a.mp3"), b = addRecording("b.mp3");
        s.enqueue(a); s.enqueue(b);
        QTest::qWait(100); // let `a` get going
        QElapsedTimer t; t.start();
        QVERIFY(s.cancel(a));
        QVERIFY(s.waitUntilIdle(10000));
        QCOMPARE(field(a, "status"), QString("new"));
        QCOMPARE(transcriptCount(a), 0);
        QCOMPARE(jobField(a, "state"), QString("cancelled"));
        QCOMPARE(field(b, "status"), QString("done"));
    }
    void pauseLetsRunningFinishButStartsNothingNew()
    {
        g_stepMs = 10;
        JobScheduler s(db, dec, factory());
        const qint64 a = addRecording("a.mp3"), b = addRecording("b.mp3"), c = addRecording("c.mp3");
        s.enqueue(a); s.enqueue(b); s.enqueue(c);
        s.setPaused(true);
        QVERIFY(s.isPaused());
        QVERIFY(!s.waitUntilIdle(3000)); // a finishes, b/c stay queued
        QCOMPARE(field(a, "status"), QString("done"));
        QCOMPARE(field(b, "status"), QString("queued"));
        QCOMPARE(s.queuedCount(), 2);
        s.setPaused(false);
        QVERIFY(s.waitUntilIdle(10000));
        QCOMPARE(field(b, "status"), QString("done"));
        QCOMPARE(field(c, "status"), QString("done"));
    }
    void duplicateEnqueueRunsOnce()
    {
        JobScheduler s(db, dec, factory());
        const qint64 id = addRecording("a.mp3");
        QVERIFY(s.enqueue(id));
        QVERIFY(s.enqueue(id)); // running
        QVERIFY(s.waitUntilIdle(5000));
        QCOMPARE(transcriptCount(id), 1);
        QVERIFY(!s.enqueue(9999)); // unknown recording
    }
    void enqueueAllNewSkipsDoneErrorAndQueued()
    {
        JobScheduler s(db, dec, factory());
        s.setPaused(true);
        addRecording("n1.mp3"); addRecording("n2.mp3");
        addRecording("d.mp3", "done"); addRecording("e.mp3", "error");
        QCOMPARE(s.enqueueAllNew(), 2);
        QCOMPARE(s.queuedCount(), 2);
        QCOMPARE(s.enqueueAllNew(), 0); // statuses are 'queued' now
    }
    void reanalysisOfDoneRecordingAddsAGeneration()
    {
        JobScheduler s(db, dec, factory());
        const qint64 id = addRecording("a.mp3");
        s.enqueue(id);
        QVERIFY(s.waitUntilIdle(5000));
        s.enqueue(id);
        QVERIFY(s.waitUntilIdle(5000));
        QCOMPARE(transcriptCount(id), 2);
        QCOMPARE(field(id, "status"), QString("done"));
    }
    void resumeInterruptedPicksUpLeftovers()
    {
        const qint64 a = addRecording("a.mp3", "transcribing"), b = addRecording("b.mp3", "queued"),
                     c = addRecording("c.mp3", "done"), d = addRecording("d.mp3", "new");
        JobScheduler s(db, dec, factory());
        QCOMPARE(s.resumeInterrupted(), 2);
        QVERIFY(s.waitUntilIdle(5000));
        QCOMPARE(field(a, "status"), QString("done"));
        QCOMPARE(field(b, "status"), QString("done"));
        QCOMPARE(transcriptCount(c), 0);
        QCOMPARE(field(d, "status"), QString("new"));
    }
    void destroyingSchedulerMidJobAbortsQuicklyAndRequeues()
    {
        g_stepMs = 200; // a job would take seconds
        const qint64 a = addRecording("a.mp3"), b = addRecording("b.mp3");
        QElapsedTimer t;
        {
            JobScheduler s(db, dec, factory());
            s.enqueue(a); s.enqueue(b);
            QTest::qWait(150);
            t.start();
        }
        QVERIFY2(t.elapsed() < 1500, qPrintable(QString::number(t.elapsed())));
        QCOMPARE(field(a, "status"), QString("queued")); // interrupted -> resumable
        QCOMPARE(field(b, "status"), QString("queued"));
        QCOMPARE(transcriptCount(a), 0);
        g_stepMs = 5;
        JobScheduler again(db, dec, factory());
        QCOMPARE(again.resumeInterrupted(), 2);
        QVERIFY(again.waitUntilIdle(5000));
        QCOMPARE(field(a, "status"), QString("done"));
    }
    void configIsPassedToTheEngine()
    {
        JobScheduler s(db, dec, factory());
        s.setConfig({ "/models/x.bin", "en", 3 });
        s.enqueue(addRecording("a.mp3"));
        QVERIFY(s.waitUntilIdle(5000));
        QCOMPARE(g_lastModel, QString("/models/x.bin"));
        QCOMPARE(g_lastOptions.language, QString("en"));
        QCOMPARE(g_lastOptions.threads, 3);
        QCOMPARE(g_lastOptions.beamSize, AnalysisConfig::kDefaultBeamSize);   // the pipeline passes the beam width on
    }
};

QTEST_MAIN(TstJobs)
#include "tst_jobs.moc"
