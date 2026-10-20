#include <QSqlError>
#include <QSqlQuery>
#include <QtTest>

#include "core/AnalysisRepository.h"
#include "core/Database.h"
#include "core/ITokenizer.h"
#include "core/KnowledgeBuilder.h"
#include "core/RecordingDetail.h"
#include "core/RecordingEditor.h"
#include "core/SearchService.h"
#include "core/TranscriptRepository.h"

using namespace ake;

class TstEditor : public QObject {
    Q_OBJECT
    Database db;
    std::unique_ptr<ITokenizer> tok;
    qint64 rec = 0;

    qint64 addRecording(const QString& relPath, const QStringList& segments)
    {
        QSqlQuery q(db.handle());
        q.prepare("INSERT INTO recordings(folder_id,rel_path,abs_path,status,recorded_at,created_at,updated_at,duration_ms) VALUES(1,?,?,'done',1700000000,0,0,125000)");
        q.addBindValue(relPath); q.addBindValue("/f/" + relPath);
        if (!q.exec()) qFatal("%s", qPrintable(q.lastError().text()));
        const qint64 id = q.lastInsertId().toLongLong();
        TranscriptResult r; r.language = "ja"; r.model = "ggml-small";
        qint64 t = 0;
        for (const QString& s : segments) { r.segments.append({ t, t + 3000, s }); t += 3000; }
        if (!TranscriptRepository(db).save(id, r)) qFatal("save");
        if (!KnowledgeBuilder(db, *tok).build(id)) qFatal("build");
        return id;
    }
    QStringList keywordNames(qint64 id)
    {
        QStringList l;
        for (const auto& k : RecordingDetail::load(db, id).keywords) l << k.name;
        return l;
    }
    int count(const QString& sql)
    {
        QSqlQuery q(db.handle());
        return (q.exec(sql) && q.next()) ? q.value(0).toInt() : -1;
    }
    QStringList docs(const QString& query)
    {
        SearchService svc(db, *tok);
        QStringList l;
        for (const auto& h : svc.search(query).hits) l << QString::number(h.recordingId);
        return l;
    }
    static QStringList meetingText()
    {
        QStringList s;
        for (int i = 0; i < 3; ++i)
            s << "Podmanへの移行方針について議論しました。" << "rootlessの運用とsystemd連携を確認しました。" << "共有フォルダの権限管理も検討しました。";
        s << "最後に検証担当者を決めます。";
        return s;
    }

private slots:
    void init()
    {
        tok = createTokenizer();
        QVERIFY(db.open(":memory:"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("INSERT INTO folders(id,path) VALUES(1,'/f')"));
        rec = addRecording("meetings/docker.m4a", meetingText());
    }

    // ---- reading
    void detailLoadsEverythingForThePane()
    {
        const RecordingDetail d = RecordingDetail::load(db, rec);
        QVERIFY(d.valid());
        QCOMPARE(d.relPath, QString("meetings/docker.m4a"));
        QCOMPARE(d.absPath, QString("/f/meetings/docker.m4a"));
        QCOMPARE(d.status, QString("done"));
        QCOMPARE(d.durationMs, qint64(125000));
        QCOMPARE(d.recordedAt.toSecsSinceEpoch(), qint64(1700000000));
        QCOMPARE(d.language, QString("ja"));
        QCOMPARE(d.model, QString("ggml-small"));
        QCOMPARE(d.segmentCount, 10);
        QVERIFY(d.title.exists && !d.title.isUser && d.title.aiAvailable);
        QVERIFY(d.summary.exists && !d.summary.isUser);
        QVERIFY(!d.keywords.isEmpty());
        QVERIFY(d.notes.isEmpty());
        QVERIFY(!RecordingDetail::load(db, 9999).valid());
    }

    // ---- title / summary
    void userTitleBeatsTheAiOneAndCanBeRevertedOrEmptied()
    {
        RecordingEditor ed(db, *tok);
        const QString ai = RecordingDetail::load(db, rec).title.content;
        QVERIFY(ed.setTitle(rec, "  週次  スタンドアップ \n"));
        RecordingDetail d = RecordingDetail::load(db, rec);
        QCOMPARE(d.title.content, QString("週次 スタンドアップ"));        // whitespace simplified
        QVERIFY(d.title.isUser);
        QVERIFY(d.title.aiAvailable);                                      // there is something to go back to
        // a new analysis must not overwrite it
        QVERIFY(KnowledgeBuilder(db, *tok).build(rec));
        QCOMPARE(RecordingDetail::load(db, rec).title.content, QString("週次 スタンドアップ"));
        QVERIFY(ed.revertTitle(rec));
        d = RecordingDetail::load(db, rec);
        QCOMPARE(d.title.content, ai);
        QVERIFY(!d.title.isUser);
        QVERIFY(ed.setTitle(rec, "別のタイトル"));
        QVERIFY(ed.setTitle(rec, "   "));                                  // empty = back to the AI title
        QCOMPARE(RecordingDetail::load(db, rec).title.content, ai);
        QVERIFY(!ed.setTitle(9999, "x"));
    }
    void titleWithoutAnAiResultFallsBackToTheFileName()
    {
        QSqlQuery q(db.handle());
        QVERIFY(q.exec(QString("DELETE FROM analyses WHERE recording_id=%1 AND kind='title'").arg(rec)));
        RecordingEditor ed(db, *tok);
        QVERIFY(ed.setTitle(rec, "手書きタイトル"));
        RecordingDetail d = RecordingDetail::load(db, rec);
        QVERIFY(d.title.isUser && !d.title.aiAvailable);
        QVERIFY(ed.setTitle(rec, ""));                                     // nothing to revert to: no title at all
        d = RecordingDetail::load(db, rec);
        QVERIFY(!d.title.exists);
    }
    void summaryEditsWorkTheSameWay()
    {
        RecordingEditor ed(db, *tok);
        const QString ai = RecordingDetail::load(db, rec).summary.content;
        QVERIFY(ed.setSummary(rec, "要点:\n- 移行は段階的に\n- 担当を決める"));
        RecordingDetail d = RecordingDetail::load(db, rec);
        QVERIFY(d.summary.isUser);
        QVERIFY(d.summary.content.contains("段階的に\n- 担当"));            // line breaks are kept
        QVERIFY(KnowledgeBuilder(db, *tok).build(rec));
        QVERIFY(RecordingDetail::load(db, rec).summary.isUser);
        QVERIFY(ed.revertSummary(rec));
        QCOMPARE(RecordingDetail::load(db, rec).summary.content, ai);
    }
    void editedTitleAndSummaryAreSearchable()
    {
        RecordingEditor ed(db, *tok);
        QVERIFY(docs("ゼブラ").isEmpty());
        QVERIFY(ed.setTitle(rec, "ゼブラ計画"));
        QCOMPARE(docs("ゼブラ"), QStringList({ QString::number(rec) }));
        QVERIFY(ed.setSummary(rec, "キリン案も比較した"));
        QCOMPARE(docs("キリン"), QStringList({ QString::number(rec) }));
        QVERIFY(ed.revertTitle(rec));
        QVERIFY(docs("ゼブラ").isEmpty());                                  // the old text is no longer indexed
    }

    // ---- keywords
    void addedKeywordsRankFirstAndAreSearchable()
    {
        RecordingEditor ed(db, *tok);
        const int before = keywordNames(rec).size();
        QVERIFY(ed.addKeyword(rec, "  重要  案件 "));
        const RecordingDetail d = RecordingDetail::load(db, rec);
        QCOMPARE(d.keywords.size(), before + 1);
        QCOMPARE(d.keywords.first().name, QString("重要 案件"));            // simplified, first
        QVERIFY(d.keywords.first().isUser);
        QCOMPARE(docs("\"重要 案件\"").size(), 1);                          // in the search row's tag column
        // adding the same keyword again changes nothing; empty and over-long names are handled
        QVERIFY(ed.addKeyword(rec, "重要 案件"));
        QCOMPARE(RecordingDetail::load(db, rec).keywords.size(), before + 1);
        QVERIFY(!ed.addKeyword(rec, "   "));
        QVERIFY(ed.addKeyword(rec, QString(100, 'x')));
        QCOMPARE(RecordingDetail::load(db, rec).keywords.first().name.size(), RecordingEditor::kMaxKeywordChars);
        QVERIFY(!ed.addKeyword(9999, "x"));
    }
    void sameKeywordInTwoRecordingsSharesOneRow()
    {
        const qint64 other = addRecording("memos/b.wav", { "別の録音です。内容はまったく違います。" });
        RecordingEditor ed(db, *tok);
        QVERIFY(ed.addKeyword(rec, "Docker"));
        QVERIFY(ed.addKeyword(other, "docker"));          // same normalized form
        QCOMPARE(count("SELECT count(*) FROM keywords WHERE normalized='docker'"), 1);
    }
    void promotingAnAiKeywordMakesItAUserKeyword()
    {
        RecordingEditor ed(db, *tok);
        const RecordingDetail d0 = RecordingDetail::load(db, rec);
        const RecordingDetail::Kw ai = d0.keywords.last();
        QVERIFY(!ai.isUser);
        QVERIFY(ed.addKeyword(rec, ai.name));
        const RecordingDetail d = RecordingDetail::load(db, rec);
        QCOMPARE(d.keywords.size(), d0.keywords.size());
        QCOMPARE(d.keywords.first().name, ai.name);
        QVERIFY(d.keywords.first().isUser);
        // user keywords survive a new analysis
        QVERIFY(KnowledgeBuilder(db, *tok).build(rec));
        QVERIFY(RecordingDetail::load(db, rec).keywords.first().isUser);
    }
    void removedAiKeywordsStayRemovedAfterReanalysis()
    {
        RecordingEditor ed(db, *tok);
        const RecordingDetail d0 = RecordingDetail::load(db, rec);
        QVERIFY(d0.keywords.size() >= 3);
        const auto victim = d0.keywords[1];
        QVERIFY(ed.removeKeyword(rec, victim.id));
        QVERIFY(!keywordNames(rec).contains(victim.name));
        QVERIFY(KnowledgeBuilder(db, *tok).build(rec));                   // analysed again
        const QStringList after = keywordNames(rec);
        QVERIFY2(!after.contains(victim.name), qPrintable(after.join(",")));
        QCOMPARE(after.size(), d0.keywords.size());                       // the list was refilled with the next candidate
        // adding it back lifts the block
        QVERIFY(ed.addKeyword(rec, victim.name));
        QVERIFY(KnowledgeBuilder(db, *tok).build(rec));
        QVERIFY(keywordNames(rec).contains(victim.name));
        QCOMPARE(count("SELECT count(*) FROM keyword_blocks"), 0);
    }
    void removingAUserKeywordDoesNotBlockIt()
    {
        RecordingEditor ed(db, *tok);
        QVERIFY(ed.addKeyword(rec, "メモ用タグ"));
        const qint64 id = RecordingDetail::load(db, rec).keywords.first().id;
        QVERIFY(ed.removeKeyword(rec, id));
        QCOMPARE(count("SELECT count(*) FROM keyword_blocks"), 0);
        QVERIFY(!keywordNames(rec).contains("メモ用タグ"));
        QVERIFY(!ed.removeKeyword(rec, id));                               // already gone
    }
    void blocksAreCleanedUpWithTheRecording()
    {
        RecordingEditor ed(db, *tok);
        QVERIFY(ed.removeKeyword(rec, RecordingDetail::load(db, rec).keywords.first().id));
        QCOMPARE(count("SELECT count(*) FROM keyword_blocks"), 1);
        QSqlQuery q(db.handle());
        QVERIFY(q.exec(QString("DELETE FROM recordings WHERE id=%1").arg(rec)));
        QCOMPARE(count("SELECT count(*) FROM keyword_blocks"), 0);
    }

    // ---- sentence corrections
    qint64 segmentId(qint64 id, int idx)
    {
        QSqlQuery q(db.handle());
        q.prepare("SELECT s.id FROM segments s JOIN transcripts t ON t.id=s.transcript_id WHERE t.recording_id=? AND t.is_current=1 AND s.idx=?");
        q.addBindValue(id); q.addBindValue(idx);
        return (q.exec() && q.next()) ? q.value(0).toLongLong() : 0;
    }
    QString editedState(qint64 seg)
    {
        QSqlQuery q(db.handle());
        q.prepare("SELECT edited_text IS NULL, COALESCE(edited_text,'') FROM segments WHERE id=?");
        q.addBindValue(seg);
        if (!q.exec() || !q.next()) return "<none>";
        return q.value(0).toBool() ? QString("<null>") : q.value(1).toString();
    }
    void sentenceCorrectionsKeepTheOriginalAndRebuildTheIndex()
    {
        RecordingEditor ed(db, *tok);
        const qint64 seg = segmentId(rec, 1);
        QVERIFY(seg > 0);
        QString err;
        QVERIFY2(ed.setSegmentText(rec, seg, "  rootlessの運用とゼブラ連携を確認しました。 ", &err), qPrintable(err));
        QCOMPARE(editedState(seg), QString("rootlessの運用とゼブラ連携を確認しました。"));   // trimmed
        QCOMPARE(docs("ゼブラ"), QStringList({ QString::number(rec) }));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec(QString("SELECT text FROM segments WHERE id=%1").arg(seg)));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QString("rootlessの運用とsystemd連携を確認しました。"));   // recognized text untouched
        {   // the old wording survives only in the other (identical) sentences of the fixture: one hit fewer than before
            SearchService svc(db, *tok);
            QCOMPARE(svc.search("systemd").hits[0].segmentHits, 2);
        }
        // the same text again: nothing to do; back to the recognized text: correction cleared
        QVERIFY(ed.setSegmentText(rec, seg, "rootlessの運用とゼブラ連携を確認しました。"));
        QVERIFY(ed.setSegmentText(rec, seg, "rootlessの運用とsystemd連携を確認しました。"));
        QCOMPARE(editedState(seg), QString("<null>"));
        {
            SearchService svc(db, *tok);
            QCOMPARE(svc.search("systemd").hits[0].segmentHits, 3);         // restored
        }
        QVERIFY(ed.setSegmentText(rec, seg, "別の文"));
        QVERIFY(ed.revertSegment(rec, seg));
        QCOMPARE(editedState(seg), QString("<null>"));
    }
    void deletingASentenceStoresAnEmptyCorrectionNotNull()
    {
        RecordingEditor ed(db, *tok);
        const qint64 seg = segmentId(rec, 2);
        QString err;
        QVERIFY2(ed.setSegmentText(rec, seg, "", &err), qPrintable(err));
        QCOMPARE(editedState(seg), QString(""));                           // "" (corrected to nothing), not NULL
        QVERIFY(ed.setSegmentText(rec, seg, "   "));                       // still empty: no change
        QCOMPARE(editedState(seg), QString(""));
        QVERIFY(ed.revertSegment(rec, seg));
        QCOMPARE(editedState(seg), QString("<null>"));
    }
    void onlySegmentsOfTheCurrentTranscriptOfThatRecordingAreEditable()
    {
        RecordingEditor ed(db, *tok);
        const qint64 oldSeg = segmentId(rec, 0);
        TranscriptResult r; r.language = "ja"; r.model = "t";
        r.segments.append({ 0, 1000, "新しい世代の文です。" });
        QVERIFY(TranscriptRepository(db).save(rec, r));                    // the first generation is no longer current
        QString err;
        QVERIFY(!ed.setSegmentText(rec, oldSeg, "古い世代は直せない", &err));
        QVERIFY(!err.isEmpty());
        const qint64 other = addRecording("memos/b.wav", { "別の録音の文です。" });
        QVERIFY(!ed.setSegmentText(rec, segmentId(other, 0), "他の録音の文は直せない"));
        QVERIFY(!ed.setSegmentText(9999, 1, "x"));
        QVERIFY(!ed.revertSegment(rec, oldSeg));
    }
    void aCorrectionRegeneratesAiTextButNotTheUsersOwn()
    {
        RecordingEditor ed(db, *tok);
        QVERIFY(ed.setTitle(rec, "手書きのタイトル"));
        const QString aiSummary = RecordingDetail::load(db, rec).summary.content;
        QVERIFY(ed.addKeyword(rec, "手動タグ"));
        QVERIFY(ed.setSegmentText(rec, segmentId(rec, 0), "キリン案とゼブラ案の比較を議論します。キリン案が有力です。キリンキリン。"));
        const RecordingDetail d = RecordingDetail::load(db, rec);
        QCOMPARE(d.title.content, QString("手書きのタイトル"));
        QVERIFY(d.keywords.first().isUser && d.keywords.first().name == "手動タグ");
        QVERIFY2(keywordNames(rec).join(",").contains("キリン"), qPrintable(keywordNames(rec).join(",")));   // regenerated from the corrected text
        QVERIFY(d.summary.content != aiSummary || d.summary.content.contains("キリン"));
    }

    // ---- favorite / notes
    void favoriteToggles()
    {
        RecordingEditor ed(db, *tok);
        QVERIFY(!RecordingDetail::load(db, rec).favorite);
        QVERIFY(ed.setFavorite(rec, true));
        QVERIFY(RecordingDetail::load(db, rec).favorite);
        QVERIFY(ed.setFavorite(rec, false));
        QVERIFY(!RecordingDetail::load(db, rec).favorite);
        QVERIFY(!ed.setFavorite(9999, true));
    }
    void notesAreSavedSearchableAndDeletedWhenEmpty()
    {
        RecordingEditor ed(db, *tok);
        QVERIFY(ed.setNotes(rec, "- Quadletを試す\n- 次回: スケジュール確認"));
        QVERIFY(RecordingDetail::load(db, rec).notes.contains("Quadlet"));
        QCOMPARE(docs("スケジュール確認").size(), 1);
        QVERIFY(ed.setNotes(rec, "更新後のメモ"));                          // upsert
        QCOMPARE(count("SELECT count(*) FROM notes"), 1);
        QVERIFY(docs("Quadlet").isEmpty());                                // the replaced note is no longer indexed
        QCOMPARE(docs("更新後").size(), 1);
        QVERIFY(ed.setNotes(rec, "  \n "));                                // empty: the note is removed
        QCOMPARE(count("SELECT count(*) FROM notes"), 0);
        QVERIFY(RecordingDetail::load(db, rec).notes.isEmpty());
        QVERIFY(!ed.setNotes(9999, "x"));                                  // unknown recording
        QCOMPARE(count("SELECT count(*) FROM notes"), 0);
    }
};

QTEST_MAIN(TstEditor)
#include "tst_editor.moc"
