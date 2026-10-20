#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include "core/Database.h"

using ake::Database;

namespace {
QVariant scalar(Database& d, const QString& sql)
{
    QSqlQuery q(d.handle());
    if (!q.exec(sql) || !q.next())
        return QVariant();
    return q.value(0);
}
bool exec(Database& d, const QString& sql, QString* err = nullptr)
{
    QSqlQuery q(d.handle());
    const bool ok = q.exec(sql);
    if (!ok && err) *err = q.lastError().text();
    return ok;
}
} // namespace

class TstDatabase : public QObject {
    Q_OBJECT
private slots:
    void createsSchemaAndRecordsVersion()
    {
        Database d;
        QString err;
        QVERIFY2(d.open(":memory:", &err), qPrintable(err));
        QCOMPARE(d.schemaVersion(), Database::latestSchemaVersion());
        for (const char* t : { "folders", "recordings", "transcripts", "segments", "analyses", "keywords",
                               "recording_keywords", "chapters", "notes", "jobs", "settings",
                               "schema_migrations", "fts_segments", "fts_recording" })
            QCOMPARE(scalar(d, QString("SELECT count(*) FROM sqlite_master WHERE name='%1'").arg(t)).toInt(), 1);
    }
    void reopeningIsIdempotent()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("t.db");
        {
            Database d;
            QVERIFY(d.open(path));
            QVERIFY(exec(d, "INSERT INTO settings(key,value) VALUES('k','v')"));
        }
        Database d;
        QVERIFY(d.open(path));
        QCOMPARE(scalar(d, "SELECT count(*) FROM schema_migrations").toInt(), Database::latestSchemaVersion());
        QCOMPARE(scalar(d, "SELECT value FROM settings WHERE key='k'").toString(), QString("v"));
        QCOMPARE(scalar(d, "PRAGMA journal_mode").toString(), QString("wal"));
    }
    void rejectsNewerSchema()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("t.db");
        {
            Database d;
            QVERIFY(d.open(path));
            QVERIFY(exec(d, "INSERT INTO schema_migrations(version,applied_at) VALUES(999,0)"));
        }
        Database d;
        QString err;
        QVERIFY(!d.open(path, &err));
        QVERIFY2(err.contains("newer"), qPrintable(err));
    }
    void foreignKeysCascadeAndCheckConstraints()
    {
        Database d;
        QVERIFY(d.open(":memory:"));
        QCOMPARE(scalar(d, "PRAGMA foreign_keys").toInt(), 1);
        QVERIFY(exec(d, "INSERT INTO folders(id,path) VALUES(1,'/a')"));
        QVERIFY(exec(d, "INSERT INTO recordings(id,folder_id,rel_path,abs_path,created_at,updated_at) "
                        "VALUES(1,1,'x.mp3','/a/x.mp3',0,0)"));
        QVERIFY(exec(d, "INSERT INTO transcripts(id,recording_id,created_at) VALUES(1,1,0)"));
        QVERIFY(exec(d, "INSERT INTO segments(transcript_id,idx,start_ms,end_ms,text) VALUES(1,0,0,1000,'hi')"));
        // bad status rejected
        QVERIFY(!exec(d, "UPDATE recordings SET status='bogus' WHERE id=1"));
        // orphan recording rejected
        QVERIFY(!exec(d, "INSERT INTO recordings(folder_id,rel_path,abs_path,created_at,updated_at) "
                         "VALUES(42,'y','/y',0,0)"));
        // deleting the folder cascades down to segments
        QVERIFY(exec(d, "DELETE FROM folders WHERE id=1"));
        QCOMPARE(scalar(d, "SELECT count(*) FROM recordings").toInt(), 0);
        QCOMPARE(scalar(d, "SELECT count(*) FROM segments").toInt(), 0);
    }
    void ftsFindsJapaneseWakati()
    {
        Database d;
        QVERIFY(d.open(":memory:"));
        // wakati text as MeCab would produce it
        QVERIFY(exec(d, "INSERT INTO fts_segments(rowid,wakati) VALUES(1,'Podman へ の 移行 は 段階 的 に 進める')"));
        QVERIFY(exec(d, "INSERT INTO fts_segments(rowid,wakati) VALUES(2,'今日 は Docker Compose の 運用 状況 を 共有 し ます')"));
        QCOMPARE(scalar(d, "SELECT rowid FROM fts_segments WHERE fts_segments MATCH '移行'").toInt(), 1);
        QCOMPARE(scalar(d, "SELECT rowid FROM fts_segments WHERE fts_segments MATCH 'docker AND 運用'").toInt(), 2);
        QCOMPARE(scalar(d, "SELECT count(*) FROM fts_segments WHERE fts_segments MATCH 'podman OR docker'").toInt(), 2);
        // case-insensitive, no match for absent word
        QCOMPARE(scalar(d, "SELECT count(*) FROM fts_segments WHERE fts_segments MATCH 'rust'").toInt(), 0);
        // snippet with highlight markers
        QCOMPARE(scalar(d, "SELECT snippet(fts_segments,0,'[',']','…',5) FROM fts_segments "
                           "WHERE fts_segments MATCH '移行'").toString().contains("[移行]"), true);
    }
    void ftsRecordingMultiColumnAndWeights()
    {
        Database d;
        QVERIFY(d.open(":memory:"));
        QVERIFY(exec(d, "INSERT INTO fts_recording(rowid,title,summary,tags,notes) "
                        "VALUES(1,'Docker 移行 会議','Compose から Podman 移行','docker linux podman','')"));
        QVERIFY(exec(d, "INSERT INTO fts_recording(rowid,title,summary,tags,notes) "
                        "VALUES(2,'勉強 会','Podman の rootless','podman systemd','')"));
        QCOMPARE(scalar(d, "SELECT count(*) FROM fts_recording WHERE fts_recording MATCH 'podman'").toInt(), 2);
        QCOMPARE(scalar(d, "SELECT rowid FROM fts_recording WHERE fts_recording MATCH 'title:移行'").toInt(), 1);
        // bm25 weights (title > tags > summary > notes): title hit outranks summary-only hit
        QCOMPARE(scalar(d, "SELECT rowid FROM fts_recording WHERE fts_recording MATCH '移行 OR rootless' "
                           "ORDER BY bm25(fts_recording,10.0,2.0,5.0,1.0) LIMIT 1").toInt(), 1);
    }
};

QTEST_MAIN(TstDatabase)
#include "tst_database.moc"
