#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <atomic>
#include <mutex>
#include <thread>

#include "core/Database.h"
#include "core/Library.h"
#include "core/PollingFolderWatcher.h"
#include "core/ScanService.h"

using namespace ake;

namespace {
// Decoder whose probe() is slow and counts its calls: exposes blocking, and which files were probed.
struct SlowDecoder : IDecoder {
    int delayMs = 0;
    std::atomic<int> probes{ 0 };
    QStringList probed;
    std::mutex mu;
    bool probe(const QString& path, AudioInfo* i, QString*) override
    {
        ++probes;
        { std::lock_guard<std::mutex> l(mu); probed << QFileInfo(path).fileName(); }
        if (delayMs) QThread::msleep(delayMs);
        i->durationMs = 7000; i->sampleRate = 16000;
        return true;
    }
    bool decode(const QString&, std::vector<float>*, AudioInfo*, QString*, const Progress&) override { return false; }
};
void write(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(path));
    f.write(data);
}
QByteArray bytes(const char* tag) { return QByteArray(tag) + QByteArray(3000, 'x'); }
} // namespace

class TstScan : public QObject {
    Q_OBJECT
    Database db;
    QTemporaryDir dir;
    SlowDecoder dec;
    std::unique_ptr<Library> lib;
    std::unique_ptr<ScanService> svc;
    QString root(const QString& n) const { return dir.path() + "/" + n; }
    int count(const QString& sql)
    {
        QSqlQuery q(db.handle());
        return (q.exec(sql) && q.next()) ? q.value(0).toInt() : -1;
    }

private slots:
    void init()
    {
        QDir(dir.path()).removeRecursively();
        QDir().mkpath(dir.path());
        QVERIFY(db.open(":memory:"));
        dec.delayMs = 0; dec.probes = 0; dec.probed.clear();
        lib = std::make_unique<Library>(db);
        svc = std::make_unique<ScanService>(db, &dec);
    }
    void cleanup() { svc.reset(); lib.reset(); }

    // ---- the point: the UI thread stays responsive
    void aSlowScanDoesNotBlockTheEventLoop()
    {
        for (int i = 0; i < 8; ++i) write(root("rec") + QString("/f%1.mp3").arg(i), bytes(QByteArray::number(i)));
        const qint64 id = lib->addFolder(root("rec"));
        dec.delayMs = 120;                                                // 8 files x 120 ms = ~1 s of work
        int ticks = 0;
        QTimer t; t.setInterval(20); connect(&t, &QTimer::timeout, [&] { ++ticks; }); t.start();
        QSignalSpy started(svc.get(), &ScanService::started), done(svc.get(), &ScanService::finished);
        QElapsedTimer e; e.start();
        svc->scanFolder(id);
        QCOMPARE(started.count(), 1);                                     // announced at once ...
        QVERIFY(svc->isScanning());
        QCOMPARE(count("SELECT count(*) FROM recordings"), 0);            // ... but nothing is written yet
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        QVERIFY2(ticks >= 15, qPrintable(QString("the event loop ran only %1 times in %2 ms").arg(ticks).arg(e.elapsed())));
        const ScanResult r = done.at(0).at(1).value<ScanResult>();
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.added, 8);
        QCOMPARE(count("SELECT count(*) FROM recordings"), 8);
        QCOMPARE(count("SELECT count(*) FROM recordings WHERE duration_ms=7000"), 8);   // probed metadata was stored
        QVERIFY(!svc->isScanning());
    }

    // ---- queue
    void foldersRunOneAtATimeInOrderAndDuplicatesAreIgnored()
    {
        write(root("a") + "/1.mp3", bytes("a")); write(root("b") + "/1.mp3", bytes("b")); write(root("c") + "/1.mp3", bytes("c"));
        const qint64 a = lib->addFolder(root("a")), b = lib->addFolder(root("b")), c = lib->addFolder(root("c"));
        dec.delayMs = 60;
        QList<qint64> order;
        connect(svc.get(), &ScanService::started, [&](qint64 id) { order << id; });
        QSignalSpy idle(svc.get(), &ScanService::idle);
        svc->scanFolder(a); svc->scanFolder(b); svc->scanFolder(a); svc->scanFolder(c); svc->scanFolder(b);
        QCOMPARE(svc->pending(), 3);                                      // a (running), b, c: duplicates dropped
        QVERIFY(svc->waitUntilIdle(10000));
        QCOMPARE(order, QList<qint64>({ a, b, c }));
        QCOMPARE(idle.count(), 1);
        svc->scanFolder(a);                                               // once finished it may be queued again
        QVERIFY(svc->waitUntilIdle(5000));
        QCOMPARE(order.size(), 4);
    }
    void scanAllCoversEnabledFoldersOnly()
    {
        write(root("a") + "/1.mp3", bytes("a")); write(root("b") + "/1.mp3", bytes("b"));
        const qint64 a = lib->addFolder(root("a")), b = lib->addFolder(root("b"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec(QString("UPDATE folders SET enabled=0 WHERE id=%1").arg(b)));
        svc->scanAll();
        QVERIFY(svc->waitUntilIdle(5000));
        QCOMPARE(lib->recordingCountInFolder(a), 1);
        QCOMPARE(lib->recordingCountInFolder(b), 0);
    }

    // ---- results are the same as the synchronous scan
    void changesAreDetectedLikeTheSynchronousScan()
    {
        write(root("r") + "/keep.mp3", bytes("k")); write(root("r") + "/gone.mp3", bytes("g"));
        write(root("r") + "/edit.mp3", bytes("e")); write(root("r") + "/old.mp3", bytes("moved"));
        const qint64 id = lib->addFolder(root("r"));
        auto run = [&] { QSignalSpy d(svc.get(), &ScanService::finished); svc->scanFolder(id); [&] { QVERIFY(svc->waitUntilIdle(5000)); }(); return d.at(0).at(1).value<ScanResult>(); };
        ScanResult r = run();
        QCOMPARE(r.added, 4);
        QCOMPARE(dec.probes.load(), 4);
        dec.probes = 0; dec.probed.clear();
        r = run();                                                          // nothing changed
        QCOMPARE(r.unchanged, 4);
        QCOMPARE(dec.probes.load(), 0);                                     // unchanged files are not even opened
        QVERIFY(QFile::remove(root("r") + "/gone.mp3"));
        write(root("r") + "/edit.mp3", bytes("edited-longer"));
        QVERIFY(QFile::rename(root("r") + "/old.mp3", root("r") + "/renamed.mp3"));
        write(root("r") + "/new.mp3", bytes("n"));
        r = run();
        QCOMPARE(r.added, 1); QCOMPARE(r.updated, 1); QCOMPARE(r.moved, 1); QCOMPARE(r.removed, 1); QCOMPARE(r.unchanged, 1);
        QStringList probed = dec.probed; probed.sort();
        QCOMPARE(probed, QStringList({ "edit.mp3", "new.mp3" }));            // the moved file keeps its stored metadata: no probe
        QCOMPARE(count("SELECT count(*) FROM recordings"), 4);
    }

    // ---- failures
    void anUnmountedFolderReportsAnErrorKeepsItsRecordsAndDoesNotStopTheQueue()
    {
        write(root("a") + "/1.mp3", bytes("a")); write(root("b") + "/1.mp3", bytes("b"));
        const qint64 a = lib->addFolder(root("a")), b = lib->addFolder(root("b"));
        svc->scanAll();
        QVERIFY(svc->waitUntilIdle(5000));
        QCOMPARE(count("SELECT count(*) FROM recordings"), 2);
        QVERIFY(QDir(root("a")).removeRecursively());                       // "unplug the drive"
        QList<ScanResult> results; QList<qint64> ids;
        connect(svc.get(), &ScanService::finished, [&](qint64 id, const ScanResult& r) { ids << id; results << r; });
        svc->scanFolder(a); svc->scanFolder(b);
        QVERIFY(svc->waitUntilIdle(5000));
        QCOMPARE(ids, QList<qint64>({ a, b }));
        QVERIFY(!results[0].ok());
        QVERIFY(results[0].error.contains("not accessible"));
        QVERIFY(results[1].ok());                                           // the next folder was still scanned
        QCOMPARE(lib->recordingCountInFolder(a), 1);                        // nothing was deleted
    }
    void aFolderRemovedWhileItsScanRunsLeavesTheDatabaseConsistent()
    {
        for (int i = 0; i < 4; ++i) write(root("r") + QString("/f%1.mp3").arg(i), bytes(QByteArray::number(i)));
        const qint64 id = lib->addFolder(root("r"));
        dec.delayMs = 150;
        QSignalSpy done(svc.get(), &ScanService::finished);
        svc->scanFolder(id);
        QTest::qWait(60);
        QVERIFY(lib->removeFolder(id));                                     // the user removes it mid-scan
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
        QVERIFY(!done.at(0).at(1).value<ScanResult>().ok());                // the insert is refused (foreign key) ...
        QCOMPARE(count("SELECT count(*) FROM recordings"), 0);              // ... and nothing half-written remains
        QCOMPARE(count("SELECT count(*) FROM folders"), 0);
        // a scan request for a folder that no longer exists is an error, not a crash
        QSignalSpy again(svc.get(), &ScanService::finished);
        svc->scanFolder(id);
        QVERIFY(svc->waitUntilIdle(3000));
        QCOMPARE(again.count(), 1);
        QVERIFY(again.at(0).at(1).value<ScanResult>().error.contains("no such folder"));
    }
    void destroyingTheServiceMidScanWaitsForTheWorkerAndIsSafe()
    {
        for (int i = 0; i < 4; ++i) write(root("r") + QString("/f%1.mp3").arg(i), bytes(QByteArray::number(i)));
        lib->addFolder(root("r"));
        dec.delayMs = 100;
        svc->scanAll();
        QTest::qWait(30);
        QElapsedTimer t; t.start();
        svc.reset();                                                        // must not crash with the worker still probing
        QVERIFY2(t.elapsed() < 3000, qPrintable(QString::number(t.elapsed())));
        QCOMPARE(count("SELECT count(*) FROM recordings"), 0);              // nothing was applied after the service was gone
    }

    // ---- inspect() is pure
    void inspectCanRunOnSeveralThreadsAtOnce()
    {
        for (int i = 0; i < 6; ++i) { write(root("x") + QString("/f%1.mp3").arg(i), bytes(QByteArray::number(i))); write(root("y") + QString("/g%1.mp3").arg(i), bytes(QByteArray::number(i + 50))); }
        const qint64 x = lib->addFolder(root("x")), y = lib->addFolder(root("y"));
        const ScanInput ix = lib->prepareScan(x), iy = lib->prepareScan(y);
        ScanOutput ox, oy;
        std::thread t1([&] { ox = Library::inspect(ix, &dec); }), t2([&] { oy = Library::inspect(iy, &dec); });
        t1.join(); t2.join();
        QCOMPARE(ox.files.size(), 6);
        QCOMPARE(oy.files.size(), 6);
        QCOMPARE(ox.facts.size(), 6);
        QCOMPARE(count("SELECT count(*) FROM recordings"), 0);              // inspect never writes
        QCOMPARE(lib->applyScan(ix, ox).added, 6);
        QCOMPARE(lib->applyScan(iy, oy).added, 6);
    }
    void libraryHelpersForTheFolderDialog()
    {
        write(root("r") + "/a.mp3", bytes("a")); write(root("r") + "/sub/b.mp3", bytes("b")); write(root("r") + "/skip.tmp.mp3", bytes("c"));
        const qint64 id = lib->addFolder(root("r"));
        QCOMPARE(lib->recordingCountInFolder(id), 0);
        lib->scanFolder(id);
        QCOMPARE(lib->recordingCountInFolder(id), 3);
        QVERIFY(lib->setFolderOptions(id, false, "*.tmp.mp3"));             // non-recursive + exclude
        const auto f = lib->folders().first();
        QVERIFY(!f.recursive);
        QCOMPARE(f.excludeGlob, QString("*.tmp.mp3"));
        const ScanResult r = lib->scanFolder(id);
        QCOMPARE(r.removed, 2);                                              // sub/b.mp3 and skip.tmp.mp3 no longer match
        QCOMPARE(lib->recordingCountInFolder(id), 1);
        QVERIFY(!lib->setFolderOptions(9999, true, ""));
        QVERIFY(lib->setFolderOptions(id, true, QString()));                 // null exclude must not violate NOT NULL
    }

    // ---- watcher
    void pollingWatcherFiresRepeatedlyUntilStopped()
    {
        PollingFolderWatcher w;
        QVERIFY(!w.isRunning());
        QSignalSpy s(&w, &IFolderWatcher::changeSuspected);
        w.startMs(25);
        QVERIFY(w.isRunning());
        QTRY_VERIFY_WITH_TIMEOUT(s.count() >= 3, 1500);
        w.stop();
        QVERIFY(!w.isRunning());
        const int n = s.count();
        QTest::qWait(120);
        QCOMPARE(s.count(), n);                                              // silent after stop()
        w.start(0);                                                          // <= 0 means stopped
        QVERIFY(!w.isRunning());
        w.start(2);
        QVERIFY(w.isRunning());
        QCOMPARE(w.intervalMs(), 2000);
        w.startMs(40);                                                       // changing the interval restarts it
        QCOMPARE(w.intervalMs(), 40);
        w.start(-5);
        QVERIFY(!w.isRunning());
    }
};

QTEST_MAIN(TstScan)
#include "tst_scan.moc"
