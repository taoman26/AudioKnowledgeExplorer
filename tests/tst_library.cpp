#include <QDir>
#include <QFile>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include "core/Database.h"
#include "core/FFmpegDecoder.h"
#include "core/FolderScanner.h"
#include "core/IDecoder.h"
#include "core/Library.h"

using namespace ake;

namespace {
// Dummy "audio": scanning never decodes, so distinct bytes with the right extension are enough.
void writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(path));
    f.write(data);
}
QByteArray bytes(const char* tag, int n = 4096) { return QByteArray(tag) + QByteArray(n, 'x'); }

QStringList relPaths(Database& db)
{
    QStringList l;
    QSqlQuery q(db.handle());
    q.exec("SELECT rel_path FROM recordings ORDER BY rel_path");
    while (q.next()) l << q.value(0).toString();
    return l;
}
QVariant one(Database& db, const QString& sql)
{
    QSqlQuery q(db.handle());
    return (q.exec(sql) && q.next()) ? q.value(0) : QVariant();
}

struct FakeDecoder : IDecoder {
    int probes = 0;
    bool probe(const QString& path, AudioInfo* i, QString*) override
    {
        probes++;
        if (path.endsWith("bad.mp3")) return false;
        i->durationMs = path.endsWith("long.mp3") ? 3600 * 1000 : 42 * 1000;
        i->sampleRate = 44100;
        if (path.endsWith("tagged.mp3")) i->creationTime = QDateTime(QDate(2026, 3, 1), QTime(10, 0), Qt::UTC);
        return true;
    }
    bool decode(const QString&, std::vector<float>*, AudioInfo*, QString*, const Progress&) override { return false; }
};
} // namespace

class TstLibrary : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    QString root() const { return m_dir.path() + "/rec"; }
    Database db;
    std::unique_ptr<Library> lib;

private slots:
    void init()
    {
        QDir(m_dir.path()).removeRecursively();
        QDir().mkpath(m_dir.path());
        QVERIFY(db.open(":memory:"));
        lib = std::make_unique<Library>(db);
    }

    void scannerFiltersByExtensionHiddenAndEmpty()
    {
        writeFile(root() + "/a.mp3", bytes("a"));
        writeFile(root() + "/B.WAV", bytes("b"));          // extension is case-insensitive
        writeFile(root() + "/c.m4a", bytes("c"));
        writeFile(root() + "/d.flac", bytes("d"));
        writeFile(root() + "/e.ogg", bytes("e"));
        writeFile(root() + "/notes.txt", "text");           // unsupported
        writeFile(root() + "/screen.MP4", bytes("v"));      // a meeting recorded as video: its sound is read
        writeFile(root() + "/call.webm", bytes("w"));
        writeFile(root() + "/clip.avi", bytes("x"));        // unsupported container
        writeFile(root() + "/.hidden.mp3", bytes("h"));     // hidden file
        writeFile(root() + "/.Trash-1000/t.mp3", bytes("t")); // hidden dir
        writeFile(root() + "/empty.mp3", "");                // empty
        QList<ScanFile> out;
        QVERIFY(FolderScanner::scan(root(), true, {}, &out));
        QStringList got;
        for (auto& f : out) got << f.relPath;
        got.sort();
        QCOMPARE(got, QStringList({ "B.WAV", "a.mp3", "c.m4a", "call.webm", "d.flac", "e.ogg", "screen.MP4" }));
        QCOMPARE(out.first().format.size() >= 3, true);
    }
    void scannerRecursiveAndExclude()
    {
        writeFile(root() + "/top.mp3", bytes("1"));
        writeFile(root() + "/sub/deep.mp3", bytes("2"));
        writeFile(root() + "/sub/x_backup.mp3", bytes("3"));
        writeFile(root() + "/skipme/y.mp3", bytes("4"));
        QList<ScanFile> out;
        QVERIFY(FolderScanner::scan(root(), false, {}, &out));
        QCOMPARE(out.size(), 1);
        QVERIFY(FolderScanner::scan(root(), true, FolderScanner::parseGlobList("*_backup.*, SKIPME"), &out));
        QStringList got;
        for (auto& f : out) got << f.relPath;
        got.sort();
        QCOMPARE(got, QStringList({ "sub/deep.mp3", "top.mp3" }));
    }
    void scannerReportsMissingRoot()
    {
        QList<ScanFile> out;
        QString err;
        QVERIFY(!FolderScanner::scan(m_dir.path() + "/nope", true, {}, &out, &err));
        QVERIFY(err.contains("not accessible"));
    }

    void addFolderValidation()
    {
        QDir().mkpath(root() + "/sub");
        QString err;
        QVERIFY(lib->addFolder(root(), true, "", &err) > 0);
        QVERIFY(lib->addFolder(root(), true, "", &err) == 0);           // duplicate
        QVERIFY2(err.contains("already"), qPrintable(err));
        QVERIFY(lib->addFolder(root() + "/sub", true, "", &err) == 0);  // child of registered
        QVERIFY2(err.contains("overlap"), qPrintable(err));
        QVERIFY(lib->addFolder(m_dir.path(), true, "", &err) == 0);     // parent of registered
        QVERIFY(lib->addFolder(m_dir.path() + "/missing", true, "", &err) == 0);
        QVERIFY2(err.contains("not a directory"), qPrintable(err));
        QCOMPARE(lib->folders().size(), 1);
    }

    void addFolderWithDefaultArgsSucceeds() // regression: null excludeGlob violated the NOT NULL column
    {
        QDir().mkpath(root());
        QString err;
        QVERIFY2(lib->addFolder(root(), true, QString(), &err) > 0, qPrintable(err));
        QCOMPARE(lib->folders().first().excludeGlob, QString(""));
    }
    void scanAddsThenIsStable()
    {
        writeFile(root() + "/a.mp3", bytes("a"));
        writeFile(root() + "/sub/b.wav", bytes("b"));
        QString addErr;
        const qint64 id = lib->addFolder(root(), true, "", &addErr);
        QVERIFY2(id > 0, qPrintable(addErr));
        ScanResult r = lib->scanFolder(id);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.added, 2);
        QCOMPARE(lib->recordingCount(), 2);
        QCOMPARE(relPaths(db), QStringList({ "a.mp3", "sub/b.wav" }));
        QCOMPARE(one(db, "SELECT format FROM recordings WHERE rel_path='sub/b.wav'").toString(), QString("wav"));
        QCOMPARE(one(db, "SELECT status FROM recordings WHERE rel_path='a.mp3'").toString(), QString("new"));
        QVERIFY(!one(db, "SELECT content_hash FROM recordings LIMIT 1").toString().isEmpty());
        QVERIFY(lib->folders().first().lastScanAt > 0);
        r = lib->scanFolder(id); // nothing changed
        QCOMPARE(r.added + r.updated + r.moved + r.removed, 0);
        QCOMPARE(r.unchanged, 2);
    }
    void scanPicksUpNewAndDeletedFiles()
    {
        writeFile(root() + "/a.mp3", bytes("a"));
        writeFile(root() + "/b.mp3", bytes("b"));
        QString addErr;
        const qint64 id = lib->addFolder(root(), true, "", &addErr);
        QVERIFY2(id > 0, qPrintable(addErr));
        QCOMPARE(lib->scanFolder(id).added, 2);
        writeFile(root() + "/c.ogg", bytes("c"));
        QVERIFY(QFile::remove(root() + "/a.mp3"));
        const ScanResult r = lib->scanFolder(id);
        QCOMPARE(r.added, 1);
        QCOMPARE(r.removed, 1);
        QCOMPARE(relPaths(db), QStringList({ "b.mp3", "c.ogg" }));
    }
    void modifiedFileIsMarkedForReanalysis()
    {
        writeFile(root() + "/a.mp3", bytes("a"));
        const qint64 id = lib->addFolder(root());
        lib->scanFolder(id);
        QSqlQuery(db.handle()).exec("UPDATE recordings SET status='done'");
        const QString oldHash = one(db, "SELECT content_hash FROM recordings").toString();
        writeFile(root() + "/a.mp3", bytes("a", 9000)); // different size
        const ScanResult r = lib->scanFolder(id);
        QCOMPARE(r.updated, 1);
        QCOMPARE(one(db, "SELECT status FROM recordings").toString(), QString("new"));
        QVERIFY(one(db, "SELECT content_hash FROM recordings").toString() != oldHash);
        QCOMPARE(lib->recordingCount(), 1);
    }
    void renamedOrMovedFileKeepsItsIdentity()
    {
        writeFile(root() + "/old.mp3", bytes("same"));
        writeFile(root() + "/other.mp3", bytes("other"));
        const qint64 id = lib->addFolder(root());
        lib->scanFolder(id);
        const qint64 recId = one(db, "SELECT id FROM recordings WHERE rel_path='old.mp3'").toLongLong();
        QSqlQuery(db.handle()).exec("UPDATE recordings SET status='done' WHERE id=" + QString::number(recId));
        QDir().mkpath(root() + "/archive");
        QVERIFY(QFile::rename(root() + "/old.mp3", root() + "/archive/renamed.mp3"));
        const ScanResult r = lib->scanFolder(id);
        QCOMPARE(r.moved, 1);
        QCOMPARE(r.added, 0);
        QCOMPARE(r.removed, 0);
        QCOMPARE(one(db, "SELECT id FROM recordings WHERE rel_path='archive/renamed.mp3'").toLongLong(), recId);
        QCOMPARE(one(db, "SELECT status FROM recordings WHERE id=" + QString::number(recId)).toString(), QString("done"));
    }
    void unmountedFolderKeepsRecords()
    {
        writeFile(root() + "/a.mp3", bytes("a"));
        const qint64 id = lib->addFolder(root());
        lib->scanFolder(id);
        QVERIFY(QDir(root()).removeRecursively()); // simulates an unmounted drive
        const ScanResult r = lib->scanFolder(id);
        QVERIFY(!r.ok());
        QCOMPARE(lib->recordingCount(), 1);
    }
    void removeFolderCascades()
    {
        writeFile(root() + "/a.mp3", bytes("a"));
        const qint64 id = lib->addFolder(root());
        lib->scanFolder(id);
        QVERIFY(lib->removeFolder(id));
        QCOMPARE(lib->recordingCount(), 0);
        QVERIFY(!lib->removeFolder(id));
    }
    // ---- metadata probing during scan (F6)
    void scanStoresProbedMetadata()
    {
        writeFile(root() + "/long.mp3", bytes("l"));
        writeFile(root() + "/tagged.mp3", bytes("t"));
        writeFile(root() + "/bad.mp3", bytes("b"));    // probe fails: still registered, zeros
        FakeDecoder fake;
        lib->setDecoder(&fake);
        const qint64 id = lib->addFolder(root());
        QCOMPARE(lib->scanFolder(id).added, 3);
        QCOMPARE(fake.probes, 3);
        QCOMPARE(one(db, "SELECT duration_ms FROM recordings WHERE rel_path='long.mp3'").toLongLong(), 3600 * 1000);
        QCOMPARE(one(db, "SELECT sample_rate FROM recordings WHERE rel_path='long.mp3'").toInt(), 44100);
        QCOMPARE(one(db, "SELECT duration_ms FROM recordings WHERE rel_path='bad.mp3'").toLongLong(), 0);
        QCOMPARE(one(db, "SELECT status FROM recordings WHERE rel_path='bad.mp3'").toString(), QString("new")); // not an error yet
        // container creation_time wins over the file's mtime for recorded_at
        QCOMPARE(one(db, "SELECT recorded_at FROM recordings WHERE rel_path='tagged.mp3'").toLongLong(),
                 QDateTime(QDate(2026, 3, 1), QTime(10, 0), Qt::UTC).toSecsSinceEpoch());
        QVERIFY(one(db, "SELECT recorded_at FROM recordings WHERE rel_path='long.mp3'").toLongLong() > 1000000000);
        // unchanged files are not probed again
        fake.probes = 0;
        QCOMPARE(lib->scanFolder(id).unchanged, 3);
        QCOMPARE(fake.probes, 0);
        // a moved file keeps its stored metadata and is not re-probed
        QVERIFY(QFile::rename(root() + "/long.mp3", root() + "/renamed.mp3"));
        QCOMPARE(lib->scanFolder(id).moved, 1);
        QCOMPARE(fake.probes, 0);
        QCOMPARE(one(db, "SELECT duration_ms FROM recordings WHERE rel_path='renamed.mp3'").toLongLong(), 3600 * 1000);
    }
    void scanWithRealFfmpegReadsWavDuration()
    {
        // minimal 16-bit mono 8 kHz WAV, 0.5 s of silence
        QByteArray wav("RIFF");
        // NB: QByteArray::append(count, ch) is (count, char) in Qt6, so append single bytes with append(char).
        auto le32 = [&](quint32 v) { for (int s = 0; s < 32; s += 8) wav.append(char((v >> s) & 0xff)); };
        auto le16 = [&](quint16 v) { wav.append(char(v & 0xff)); wav.append(char(v >> 8)); };
        const int n = 4000;
        le32(36 + n * 2); wav.append("WAVEfmt "); le32(16); le16(1); le16(1); le32(8000); le32(16000); le16(2); le16(16);
        wav.append("data"); le32(n * 2); wav.append(QByteArray(n * 2, '\0'));
        writeFile(root() + "/s.wav", wav);
        writeFile(root() + "/garbage.mp3", bytes("g"));
        FFmpegDecoder real;
        lib->setDecoder(&real);
        const ScanResult r = lib->scanFolder(lib->addFolder(root()));
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.added, 2);
        QVERIFY(qAbs(one(db, "SELECT duration_ms FROM recordings WHERE rel_path='s.wav'").toLongLong() - 500) <= 10);
        QCOMPARE(one(db, "SELECT sample_rate FROM recordings WHERE rel_path='s.wav'").toInt(), 8000);
        QCOMPARE(one(db, "SELECT duration_ms FROM recordings WHERE rel_path='garbage.mp3'").toLongLong(), 0);
    }
    void excludeGlobsStoredOnFolderApplyOnScan()
    {
        writeFile(root() + "/keep.mp3", bytes("k"));
        writeFile(root() + "/drop.tmp.mp3", bytes("d"));
        const qint64 id = lib->addFolder(root(), true, "*.tmp.mp3");
        QCOMPARE(lib->scanFolder(id).added, 1);
        QCOMPARE(relPaths(db), QStringList({ "keep.mp3" }));
    }
};

QTEST_MAIN(TstLibrary)
#include "tst_library.moc"
