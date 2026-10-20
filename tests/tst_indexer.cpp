#include <QSqlError>
#include <QSqlQuery>
#include <QtTest>

#include "core/Database.h"
#include "core/DemoData.h"
#include "core/ITokenizer.h"
#include "core/NgramTokenizer.h"
#include "core/TextIndexer.h"
#include "core/TranscriptRepository.h"
#include "ui/RecordingListModel.h"

using namespace ake;

class TstIndexer : public QObject {
    Q_OBJECT
    Database db;
    std::unique_ptr<ITokenizer> tok;

    qint64 addRecording(const QString& name)
    {
        QSqlQuery q(db.handle());
        q.prepare("INSERT INTO recordings(folder_id,rel_path,abs_path,status,created_at,updated_at) VALUES(1,?,?,'done',0,0)");
        q.addBindValue(name); q.addBindValue("/f/" + name);
        if (!q.exec()) qFatal("%s", qPrintable(q.lastError().text()));
        return q.lastInsertId().toLongLong();
    }
    qint64 transcribe(qint64 rec, const QStringList& sentences)
    {
        TranscriptResult r;
        r.language = "ja"; r.model = "t";
        qint64 t = 0;
        for (const QString& s : sentences) { r.segments.append({ t, t + 2000, s }); t += 2000; }
        QString err;
        const qint64 id = TranscriptRepository(db).save(rec, r, &err);
        if (!id) qFatal("%s", qPrintable(err));
        return id;
    }
    // FTS search the way F11 will: the query goes through the same tokenizer
    QList<qint64> searchSegments(const QString& query)
    {
        QSqlQuery q(db.handle());
        q.prepare("SELECT DISTINCT t.recording_id FROM fts_segments JOIN segments s ON s.id=fts_segments.rowid"
                  " JOIN transcripts t ON t.id=s.transcript_id WHERE fts_segments MATCH ? AND t.is_current=1 ORDER BY 1");
        q.addBindValue("\"" + tok->wakati(query) + "\"");
        QList<qint64> out;
        if (q.exec()) while (q.next()) out << q.value(0).toLongLong();
        else qWarning() << q.lastError().text();
        return out;
    }
    int count(const QString& sql)
    {
        QSqlQuery q(db.handle());
        return (q.exec(sql) && q.next()) ? q.value(0).toInt() : -1;
    }
    QStringList keywordsOf(qint64 rec, const QString& source = QString())
    {
        QSqlQuery q(db.handle());
        q.prepare("SELECT k.name FROM recording_keywords rk JOIN keywords k ON k.id=rk.keyword_id WHERE rk.recording_id=?"
                  + QString(source.isEmpty() ? "" : " AND rk.source='" + source + "'") + " ORDER BY rk.score DESC, k.name");
        q.addBindValue(rec);
        QStringList l;
        if (q.exec()) while (q.next()) l << q.value(0).toString();
        return l;
    }

private slots:
    void init()
    {
        tok = createTokenizer();
        QVERIFY(db.open(":memory:"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("INSERT INTO folders(id,path) VALUES(1,'/f')"));
    }

    void indexingMakesSegmentsSearchableAcrossFormsOfTheQuery()
    {
        const qint64 a = addRecording("a.mp3");
        transcribe(a, { "今日はDocker ComposeからPodmanへの移行について議論します。", "共有フォルダの権限を確認します。" });
        TextIndexer idx(db, *tok);
        TextIndexer::Result r;
        QString err;
        QVERIFY2(idx.index(a, &r, &err), qPrintable(err));
        QCOMPARE(r.segments, 2);
        QCOMPARE(count("SELECT count(*) FROM segments WHERE wakati<>''"), 2);
        QCOMPARE(count("SELECT count(*) FROM fts_segments"), 2);
        QCOMPARE(searchSegments("移行"), QList<qint64>({ a }));
        QCOMPARE(searchSegments("共有フォルダ"), QList<qint64>({ a }));          // phrase
        QCOMPARE(searchSegments("Docker Compose"), QList<qint64>({ a }));
        QCOMPARE(searchSegments("ｐｏｄｍａｎ"), QList<qint64>({ a }));          // full-width, case-insensitive
        QCOMPARE(searchSegments("フォルダ共有"), QList<qint64>());               // order matters for phrases
        QCOMPARE(searchSegments("kubernetes"), QList<qint64>());
    }
    void keywordsAreStoredWithScoresAndShownOnCards()
    {
        const qint64 a = addRecording("meeting.mp3");
        transcribe(a, { "Podmanへの移行を議論しました。", "Podmanのrootless運用が課題です。", "移行は段階的に進めます。", "共有フォルダの権限も確認しました。" });
        QVERIFY(TextIndexer(db, *tok).index(a));
        const QStringList kw = keywordsOf(a, "ai");
        QVERIFY2(!kw.isEmpty() && kw.size() <= KeywordExtractor::kDefaultTopK, qPrintable(kw.join(",")));
        QVERIFY2(kw.contains("Podman") && kw.contains("移行"), qPrintable(kw.join(",")));
        QCOMPARE(count("SELECT count(*) FROM recording_keywords WHERE score<=0"), 0);
        // card model: chips come from these keywords, best first
        RecordingListModel m(db);
        const QStringList chips = m.index(0, 0).data(RecordingListModel::KeywordsRole).toStringList();
        QCOMPARE(chips, kw);
        QCOMPARE(count("SELECT count(*) FROM fts_recording"), 1);
    }
    void reindexingIsIdempotentAndKeepsUserKeywords()
    {
        const qint64 a = addRecording("a.mp3");
        transcribe(a, { "Podmanの移行計画。", "Podmanの検証。" });
        TextIndexer idx(db, *tok);
        QVERIFY(idx.index(a));
        const QStringList first = keywordsOf(a, "ai");
        const int ftsRows = count("SELECT count(*) FROM fts_segments");
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("INSERT INTO keywords(name,normalized) VALUES('重要','重要')"));
        QVERIFY(q.exec("INSERT INTO recording_keywords(recording_id,keyword_id,score,source) SELECT " + QString::number(a) + ",id,9,'user' FROM keywords WHERE name='重要'"));
        QVERIFY(idx.index(a));
        QVERIFY(idx.index(a));
        QCOMPARE(keywordsOf(a, "ai"), first);
        QCOMPARE(keywordsOf(a, "user"), QStringList({ "重要" }));
        QCOMPARE(count("SELECT count(*) FROM fts_segments"), ftsRows);   // no duplicate rows
        QCOMPARE(count("SELECT count(*) FROM fts_recording"), 1);
    }
    void userCorrectionsAreIndexedInsteadOfTheRecognizedText()
    {
        const qint64 a = addRecording("a.mp3");
        transcribe(a, { "チャットGPGの利用について" });                       // a misrecognition
        TextIndexer idx(db, *tok);
        QVERIFY(idx.index(a));
        QCOMPARE(searchSegments("GPG"), QList<qint64>({ a }));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("UPDATE segments SET edited_text='チャットGPTの利用について'"));
        QVERIFY(idx.index(a));
        QCOMPARE(searchSegments("GPT"), QList<qint64>({ a }));
        QCOMPARE(searchSegments("GPG"), QList<qint64>());
    }
    void newTranscriptGenerationReplacesOldSearchRows()
    {
        const qint64 a = addRecording("a.mp3");
        transcribe(a, { "古い内容のDocker話" });
        TextIndexer idx(db, *tok);
        QVERIFY(idx.index(a));
        QCOMPARE(searchSegments("Docker"), QList<qint64>({ a }));
        transcribe(a, { "新しい内容のPodman話" });                           // re-analysis
        QVERIFY(idx.index(a));
        QCOMPARE(searchSegments("Podman"), QList<qint64>({ a }));
        QCOMPARE(searchSegments("Docker"), QList<qint64>());                 // old generation no longer matches
        QCOMPARE(count("SELECT count(*) FROM fts_segments"), 1);
    }
    void rareTermsOutrankCommonOnesUsingTheRestOfTheLibrary()
    {
        for (int i = 0; i < 5; ++i) {
            const qint64 other = addRecording(QString("o%1.mp3").arg(i));
            transcribe(other, { "会議の議事録です。", "会議で決まりました。" });
            QVERIFY(TextIndexer(db, *tok).index(other));
        }
        const qint64 a = addRecording("new.mp3");
        transcribe(a, { "会議の話。", "会議の続き。", "会議のまとめ。", "Podmanの話。", "Podmanの続き。", "Podmanのまとめ。" });
        TextIndexer::Result r;
        QVERIFY(TextIndexer(db, *tok).index(a, &r));
        QVERIFY2(r.keywords.size() >= 2, "need both keywords");
        QCOMPARE(r.keywords[0].term, QString("Podman"));           // appears nowhere else
        QCOMPARE(r.keywords[1].term, QString("会議"));
        QVERIFY(r.keywords[0].score > r.keywords[1].score);
    }
    void rebuildRecordingFtsUsesTitleSummaryTagsAndNotes()
    {
        const qint64 a = addRecording("meeting_2026.mp3");
        transcribe(a, { "Podmanの移行。", "Podmanの検証。" });
        TextIndexer idx(db, *tok);
        QVERIFY(idx.index(a));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec(QString("INSERT INTO analyses(recording_id,kind,content,source,created_at) VALUES(%1,'title','Docker移行会議','ai-extractive',0)").arg(a)));
        QVERIFY(q.exec(QString("INSERT INTO analyses(recording_id,kind,content,source,created_at) VALUES(%1,'summary','Composeからの移行を議論','ai-extractive',0)").arg(a)));
        QVERIFY(q.exec(QString("INSERT INTO notes(recording_id,body_md,updated_at) VALUES(%1,'次回はQuadletを試す',0)").arg(a)));
        QVERIFY(idx.rebuildRecordingFts(a));
        auto hits = [&](const QString& match) {
            q.prepare("SELECT count(*) FROM fts_recording WHERE fts_recording MATCH ?");
            q.addBindValue(match);
            return (q.exec() && q.next()) ? q.value(0).toInt() : -1;
        };
        QCOMPARE(hits("title:docker"), 1);
        QCOMPARE(hits("summary:compose"), 1);
        QCOMPARE(hits("notes:quadlet"), 1);
        QCOMPARE(hits("tags:podman"), 1);
        QCOMPARE(hits("title:podman"), 0);
        QCOMPARE(count("SELECT count(*) FROM fts_recording"), 1);
        QVERIFY(!idx.rebuildRecordingFts(9999));                          // unknown recording
    }
    void deletingRecordingsOrFoldersCleansUpSearchRows()
    {
        const qint64 a = addRecording("a.mp3"), b = addRecording("b.mp3");
        transcribe(a, { "Docker" }); transcribe(b, { "Podman" });
        TextIndexer idx(db, *tok);
        QVERIFY(idx.index(a)); QVERIFY(idx.index(b));
        QCOMPARE(count("SELECT count(*) FROM fts_segments"), 2);
        QCOMPARE(count("SELECT count(*) FROM fts_recording"), 2);
        QSqlQuery q(db.handle());
        QVERIFY(q.exec(QString("DELETE FROM recordings WHERE id=%1").arg(a)));
        QCOMPARE(count("SELECT count(*) FROM fts_segments"), 1);      // cascade segments -> trigger
        QCOMPARE(count("SELECT count(*) FROM fts_recording"), 1);
        QVERIFY(q.exec("DELETE FROM folders WHERE id=1"));            // cascades through everything
        QCOMPARE(count("SELECT count(*) FROM fts_segments"), 0);
        QCOMPARE(count("SELECT count(*) FROM fts_recording"), 0);
    }
    void indexingWithoutTranscriptFailsCleanly()
    {
        const qint64 a = addRecording("a.mp3");
        QString err;
        QVERIFY(!TextIndexer(db, *tok).index(a, nullptr, &err));
        QCOMPARE(err, QString("no transcript to index"));
        QCOMPARE(count("SELECT count(*) FROM fts_segments"), 0);
        QCOMPARE(count("SELECT count(*) FROM recording_keywords"), 0);
    }
    void silentOrEmptyTranscriptsAreFine()
    {
        const qint64 a = addRecording("a.mp3");
        TranscriptResult r; r.language = "ja"; r.model = "t";
        QVERIFY(TranscriptRepository(db).save(a, r) > 0);                // no segments at all
        TextIndexer::Result res;
        QVERIFY(TextIndexer(db, *tok).index(a, &res));
        QCOMPARE(res.segments, 0);
        QVERIFY(res.keywords.isEmpty());
        QVERIFY(keywordsOf(a).isEmpty());
    }
    void fallbackTokenizerAlsoIndexesAndSearches()
    {
        tok = std::make_unique<NgramTokenizer>();              // what a machine without MeCab uses
        const qint64 a = addRecording("a.mp3"), b = addRecording("b.mp3");
        transcribe(a, { "共有フォルダの権限を確認します。", "Podmanへの移行を議論。" });
        transcribe(b, { "今日は天気がよいです。" });
        QVERIFY(TextIndexer(db, *tok).index(a));
        QVERIFY(TextIndexer(db, *tok).index(b));
        QCOMPARE(searchSegments("共有フォルダ"), QList<qint64>({ a }));
        QCOMPARE(searchSegments("podman"), QList<qint64>({ a }));
        QCOMPARE(searchSegments("天気"), QList<qint64>({ b }));
        QCOMPARE(searchSegments("権限確認"), QList<qint64>());        // not adjacent in the text
        QVERIFY2(keywordsOf(a, "ai").contains("Podman") || keywordsOf(a, "ai").contains("podman"), qPrintable(keywordsOf(a).join(",")));
    }
};

QTEST_MAIN(TstIndexer)
#include "tst_indexer.moc"
