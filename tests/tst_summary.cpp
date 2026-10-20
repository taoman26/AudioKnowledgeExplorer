#include <QElapsedTimer>
#include <QSqlError>
#include <QSqlQuery>
#include <QtTest>

#include "core/AnalysisRepository.h"
#include "core/Database.h"
#include "core/ExtractiveSummarizer.h"
#include "core/ITokenizer.h"
#include "core/KnowledgeBuilder.h"
#include "core/RuleBasedTitleGenerator.h"
#include "core/TranscriptCleaner.h"
#include "core/TranscriptRepository.h"
#include "ui/RecordingListModel.h"

using namespace ake;

namespace {
Keyword kw(const QString& term, double score, int count = 3)
{
    return Keyword{ term, ITokenizer::normalize(term).toLower(), term, score, count };
}

// A made-up meeting about moving from Docker to Podman: sentences 2, 4 and 6 carry the topic repeatedly.
QStringList meeting()
{
    return {
        "えー、それでは始めます。",
        "今日はDockerからPodmanへの移行方針について議論します。",
        "ありがとうございます。",
        "Podmanはrootlessで動くので、セキュリティ面のメリットが大きいです。",
        "昨日の天気はとても良かったですね。",
        "移行はCI環境のPodmanから段階的に進める計画です。",
        "最後にスケジュールを確認して、Podmanの検証担当を決めます。",
        "はい、以上です。",
    };
}
} // namespace

class TstSummary : public QObject {
    Q_OBJECT
    std::unique_ptr<ITokenizer> tok;
    Database db;

    qint64 addRecording(const QString& relPath)
    {
        QSqlQuery q(db.handle());
        q.prepare("INSERT INTO recordings(folder_id,rel_path,abs_path,status,created_at,updated_at) VALUES(1,?,?,'done',0,0)");
        q.addBindValue(relPath); q.addBindValue("/f/" + relPath);
        if (!q.exec()) qFatal("%s", qPrintable(q.lastError().text()));
        return q.lastInsertId().toLongLong();
    }
    void transcribe(qint64 rec, const QStringList& texts)
    {
        TranscriptResult r; r.language = "ja"; r.model = "t";
        qint64 t = 0;
        for (const QString& s : texts) { r.segments.append({ t, t + 2000, s }); t += 2000; }
        if (!TranscriptRepository(db).save(rec, r)) qFatal("save failed");
    }

private slots:
    void init()
    {
        tok = createTokenizer();
        QVERIFY(db.open(":memory:"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("INSERT INTO folders(id,path) VALUES(1,'/f')"));
    }

    // ---- sentence splitting
    void splitsOnJapaneseAndLatinTerminators()
    {
        QCOMPARE(ExtractiveSummarizer::splitSentences({ "今日は晴れです。明日は雨ですか？そうです！" }),
                 QStringList({ "今日は晴れです。", "明日は雨ですか？", "そうです！" }));
        QCOMPARE(ExtractiveSummarizer::splitSentences({ " And so my fellow Americans, ask not. Ask what you can do." }),
                 QStringList({ "And so my fellow Americans, ask not.", "Ask what you can do." }));
    }
    void doesNotBreakInsideDecimalsOrBeforeClosingQuotes()
    {
        QCOMPARE(ExtractiveSummarizer::splitSentences({ "価格は3.5倍になりました。" }).size(), 1);
        QCOMPARE(ExtractiveSummarizer::splitSentences({ "Version 3.5 is out. Next." }), QStringList({ "Version 3.5 is out.", "Next." }));
        QCOMPARE(ExtractiveSummarizer::splitSentences({ "彼は「行きます。」と言った。" }),
                 QStringList({ "彼は「行きます。」", "と言った。" })); // closer stays with its sentence
    }
    void joinsSegmentsIntoSentences()
    {
        // whisper cuts mid-sentence: the pieces belong together
        QCOMPARE(ExtractiveSummarizer::splitSentences({ "今日は", "Podmanへの移行について", "議論します。次に" , "進みます。" }),
                 QStringList({ "今日はPodmanへの移行について議論します。", "次に進みます。" }));
        QCOMPARE(ExtractiveSummarizer::splitSentences({ "Docker", "Compose is great." }), QStringList({ "Docker Compose is great." }));
    }
    void forcesABreakAtSegmentBoundariesWhenThereIsNoPunctuation()
    {
        QStringList segs;
        for (int i = 0; i < 20; ++i) segs << "これは句読点のない長い発話の一部です";   // 18 chars each, never a terminator
        const QStringList s = ExtractiveSummarizer::splitSentences(segs, 100);
        QVERIFY2(s.size() >= 3, qPrintable(QString::number(s.size())));
        for (const QString& x : s) QVERIFY(x.size() <= 100 + 18);
        QVERIFY(ExtractiveSummarizer::splitSentences({}).isEmpty());
        QVERIFY(ExtractiveSummarizer::splitSentences({ "", "   " }).isEmpty());
    }

    void aForcedBreakKeepsTheSegmentBoundariesAsCommasAndEndsWithAFullStop()
    {
        const QString piece = "これは句読点のない長い発話の一部です";               // 18 chars, no punctuation (beam search output)
        const QStringList s = ExtractiveSummarizer::splitSentences({ piece, piece, piece, piece }, 40);
        QCOMPARE(s, QStringList({ piece + "、" + piece + "、" + piece + "。", piece }));   // a lone last piece is left as it is
        // fragments of a word or two are not followed by a comma
        const QStringList f = ExtractiveSummarizer::splitSentences({ "接続検討を行います", "で", "です", "私", "電力会社にいた時に" }, 20);
        QCOMPARE(f, QStringList({ "接続検討を行います、でです私電力会社にいた時に。" }));
        // no double punctuation when the segment already ends with a comma, and a forced break after one becomes a full stop
        QCOMPARE(ExtractiveSummarizer::splitSentences({ "最初のほうの発話です、", "次の発話が続きます、" }, 20),
                 QStringList({ "最初のほうの発話です、次の発話が続きます。" }));
        // a piece that ends in digits or Latin letters is a pause as well
        QCOMPARE(ExtractiveSummarizer::splitSentences({ "接続検討が2件", "発注は3月になりますPodman" , "移行を進めます" }, 60),
                 QStringList({ "接続検討が2件、発注は3月になりますPodman、移行を進めます。" }));
        // sentences that end by themselves are joined exactly as before; Latin text is not given Japanese commas
        QCOMPARE(ExtractiveSummarizer::splitSentences({ "今日は", "Podmanへの移行について", "議論します。" }, 200),
                 QStringList({ "今日はPodmanへの移行について議論します。" }));
        const QStringList en = ExtractiveSummarizer::splitSentences({ "ask not what your country", "can do for you", "ask what you can do" }, 30);
        for (const QString& x : en) QVERIFY2(!x.contains(QChar(0x3001)), qPrintable(x));
    }
    void aSummaryOfUnpunctuatedSpeechReadsAsSentences()
    {
        auto tok = createTokenizer();
        QStringList segs;
        const QStringList words = { "接続検討の申請について", "電力会社に確認しています", "負担金の金額を見積もります", "工事の日程は来月になります",
                                    "担当者に連絡をお願いします", "システムの設定を変更します" };
        for (int i = 0; i < 30; ++i) segs << words[i % words.size()];
        ExtractiveSummarizer sum;
        SummaryInput in; in.segments = segs; in.tokenizer = tok.get();
        const QString out = sum.summarize(in);
        QVERIFY(!out.isEmpty());
        QVERIFY2(out.contains(QChar(0x3002)) && out.contains(QChar(0x3001)), qPrintable(out));   // not one run-on
    }

    void nonSpeechTagsAreRemovedFromTheSummaryMaterial()
    {
        QCOMPARE(TranscriptCleaner::stripNonSpeechTags("[音声なし]設計検討で[音楽]まあ進めます"), QString("設計検討でまあ進めます"));
        QCOMPARE(TranscriptCleaner::stripNonSpeechTags("今日は[BLANK_AUDIO] 晴れです"), QString("今日は 晴れです"));
        QCOMPARE(TranscriptCleaner::stripNonSpeechTags("［拍手］ありがとう【音楽】"), QString("ありがとう"));
        QCOMPARE(TranscriptCleaner::stripNonSpeechTags("普通の  文章です。"), QString("普通の  文章です。"));   // nothing to remove: untouched
        QCOMPARE(TranscriptCleaner::stripNonSpeechTags("配列[0]の話"), QString("配列の話"));                  // a short bracket is taken for a tag
        QCOMPARE(TranscriptCleaner::clean({ "[音声なし]", "最初の発言です。", "[音声なし]", " [音楽] ", "次の発言です。" }),
                 QStringList({ "最初の発言です。", "次の発言です。" }));                                          // a segment of nothing else disappears
    }
    void anAnalysisThatFindsNothingRemovesEarlierAiTextButNotTheUsers()
    {
        const qint64 id = addRecording("meetings/standup.m4a");
        transcribe(id, meeting());
        KnowledgeBuilder b(db, *tok);
        KnowledgeBuilder::Result r;
        QVERIFY(b.build(id, &r));
        AnalysisRepository repo(db);
        AnalysisRepository::Entry e;
        QVERIFY(repo.current(id, AnalysisRepository::Summary, &e) && !e.content.isEmpty());
        QVERIFY(repo.current(id, AnalysisRepository::Title, &e) && !e.content.isEmpty());
        // the recording is analysed again and now has no speech (e.g. the speech gate): nothing is said
        transcribe(id, {});
        QVERIFY(b.build(id, &r));
        QVERIFY(r.summary.isEmpty() && r.keywords.isEmpty());
        QVERIFY2(!repo.current(id, AnalysisRepository::Summary, &e), "the old AI summary must be gone");
        QVERIFY2(!repo.current(id, AnalysisRepository::Title, &e), "the old AI title must be gone");
        RecordingListModel m(db);
        QCOMPARE(m.index(0, 0).data(RecordingListModel::TitleRole).toString(), QString("standup.m4a"));   // back to the file name
        // the user's own title survives an analysis that finds nothing
        transcribe(id, meeting());
        QVERIFY(b.build(id));
        QVERIFY(repo.saveUser(id, AnalysisRepository::Title, "自分で付けた題"));
        transcribe(id, {});
        QVERIFY(b.build(id));
        QVERIFY(repo.current(id, AnalysisRepository::Title, &e));
        QCOMPARE(e.content, QString("自分で付けた題"));
    }
    void theBuiltSummaryAndKeywordsDoNotContainSilenceTags()
    {
        const qint64 id = addRecording("meetings/tags.m4a");
        transcribe(id, { "[音声なし]", "接続検討の件で電力会社に確認しています。", "[音声なし]", "負担金の金額は来週までに見積もります。[音声なし]" });
        KnowledgeBuilder::Result r;
        QVERIFY(KnowledgeBuilder(db, *tok).build(id, &r));
        QVERIFY2(!r.summary.isEmpty() && !r.summary.contains("音声なし"), qPrintable(r.summary));
        for (const Keyword& k : r.keywords) QVERIFY2(!k.term.contains("音声"), qPrintable(k.term));
    }

    // ---- cleaner: cycles of several segments (seen on a real recording: 2 phrases alternating for ten minutes)
    void cleanerDropsAlternatingLoopsButKeepsTwoRounds()
    {
        QStringList in = { "最初の話です。" };
        for (int i = 0; i < 160; ++i) in << "私は、" << "私の力を抜いて、";      // 2-cycle, both phrases short
        in << "ここから本題です。";
        QCOMPARE(TranscriptCleaner::clean(in), QStringList({ "最初の話です。", "私は、", "私の力を抜いて、", "ここから本題です。" }));
        // 3-cycle
        QStringList three;
        for (int i = 0; i < 5; ++i) three << "あ案です" << "い案です" << "う案です";
        QCOMPARE(TranscriptCleaner::clean(three), QStringList({ "あ案です", "い案です", "う案です" }));
        // two rounds are ordinary conversation
        QCOMPARE(TranscriptCleaner::clean({ "はい、", "そうですか。", "はい、", "そうですか。", "では次へ。" }).size(), 5);
        // a cycle that is broken after two rounds and resumes later is not one loop
        QCOMPARE(TranscriptCleaner::clean({ "はい、", "ええ。", "はい、", "ええ。", "別の話です。", "はい、", "ええ。" }).size(), 7);
        // the loop at the very end of the transcript
        QStringList tail = { "話です。" };
        for (int i = 0; i < 6; ++i) tail << "私は、" << "私の力を抜いて、";
        QCOMPARE(TranscriptCleaner::clean(tail).size(), 3);
        QCOMPARE(TranscriptCleaner::loopMask({}).size(), 0);
        QCOMPARE(TranscriptCleaner::loopMask({ "a", "b" }), QList<bool>({ false, false }));
    }
    void cleanerDropsRepeatedSegmentsAndLoops()
    {
        QCOMPARE(TranscriptCleaner::clean({ "移動系の人が、", "移動系の人が。", "移動系の人が、", "次の話です。" }),
                 QStringList({ "移動系の人が、", "次の話です。" }));           // same utterance, punctuation aside
        QCOMPARE(TranscriptCleaner::clean({ "はい", "はい" }).size(), 1);
        QCOMPARE(TranscriptCleaner::clean({ "A案です。", "B案です。", "A案です。" }).size(), 3); // short, not adjacent: legitimate
        QCOMPARE(TranscriptCleaner::clean({ "これは長めの発言です。", "別の発言です。", "これは長めの発言です。" }).size(), 2); // echo of a recent long one
        QCOMPARE(TranscriptCleaner::collapseLoops("担当者の方に担当者の方に担当者の方に確認します"), QString("担当者の方に確認します"));
        QCOMPARE(TranscriptCleaner::collapseLoops("普通の文章です。"), QString("普通の文章です。"));
        QVERIFY(TranscriptCleaner::clean({ "", " ", "。" }).isEmpty());
    }

    // ---- summarizer
    void picksTopicSentencesInOrderAndSkipsFillers()
    {
        ExtractiveSummarizer s;
        QList<Keyword> kws = { kw("Podman", 5), kw("移行", 4) };
        const QString out = s.summarize({ meeting(), tok.get(), kws });
        QVERIFY2(out.contains("Podman"), qPrintable(out));
        QVERIFY2(!out.contains("天気"), qPrintable(out));            // off-topic
        QVERIFY2(!out.contains("ありがとうございます") && !out.contains("以上です"), qPrintable(out));
        QVERIFY2(!out.startsWith("えー"), qPrintable(out));
        QVERIFY(out.size() <= 220);
        // chronological order: the intro sentence (if chosen) precedes the plan sentence
        const int a = out.indexOf("議論します"), b = out.indexOf("段階的に");
        if (a >= 0 && b >= 0) QVERIFY(a < b);
    }
    void respectsSentenceAndCharacterLimits()
    {
        ExtractiveSummarizer s;
        SummaryOptions one; one.maxSentences = 1;
        const QString o1 = s.summarize({ meeting(), tok.get(), {} }, one);
        QCOMPARE(o1.count("。"), 1);
        SummaryOptions tiny; tiny.maxChars = 30;
        const QString o2 = s.summarize({ meeting(), tok.get(), {} }, tiny);
        QVERIFY2(o2.size() <= 30, qPrintable(o2));
        QVERIFY(!o2.isEmpty());
    }
    void avoidsNearDuplicateSentences()
    {
        ExtractiveSummarizer s;
        QStringList segs;
        for (int i = 0; i < 4; ++i) segs << "Podmanへの移行計画について詳しく議論しました。";       // identical sentences
        segs << "rootlessのセキュリティ面のメリットも確認しました。" << "検証担当者とスケジュールを決めました。";
        const QString out = s.summarize({ segs, tok.get(), {} });
        QCOMPARE(out.count("Podmanへの移行計画"), 1);                                    // not four times
        QVERIFY(out.contains("rootless") || out.contains("検証担当"));
    }
    void isDeterministic()
    {
        ExtractiveSummarizer s;
        const QString a = s.summarize({ meeting(), tok.get(), { kw("Podman", 5) } });
        for (int i = 0; i < 5; ++i) QCOMPARE(s.summarize({ meeting(), tok.get(), { kw("Podman", 5) } }), a);
    }
    void handlesDegenerateInputs()
    {
        ExtractiveSummarizer s;
        QVERIFY(s.summarize({ {}, tok.get(), {} }).isEmpty());
        QVERIFY(s.summarize({ { "", "  " }, tok.get(), {} }).isEmpty());
        // one short utterance: nothing sentence-like, falls back to the text itself
        QCOMPARE(s.summarize({ { "こんにちは" }, tok.get(), {} }), QString("こんにちは"));
        // a single real sentence is returned whole
        const QString one = "And so my fellow Americans, ask not what your country can do for you.";
        QCOMPARE(s.summarize({ { one }, tok.get(), {} }), one);
        // no tokenizer at all must not crash
        QVERIFY(!s.summarize({ meeting(), nullptr, {} }).isEmpty());
    }
    void handlesVeryLongTranscriptsInReasonableTime()
    {
        ExtractiveSummarizer s;
        QStringList segs;
        for (int i = 0; i < 3000; ++i)
            segs << QString("話題%1についてPodmanの運用と共有フォルダの権限%2を議論しました。").arg(i % 40).arg(i % 17);
        QElapsedTimer t; t.start();
        const QString out = s.summarize({ segs, tok.get(), {} });
        QVERIFY2(t.elapsed() < 8000, qPrintable(QString::number(t.elapsed()) + " ms"));
        QVERIFY(!out.isEmpty());
        QVERIFY(out.size() <= 220);
    }

    // ---- titles
    void titleUsesTopKeywordsAndKind()
    {
        RuleBasedTitleGenerator g;
        QCOMPARE(g.title({ { kw("Docker", 10), kw("移行", 9) }, "meetings/a.m4a", {} }), QString("Docker・移行会議"));
        QCOMPARE(g.title({ { kw("Podman", 10) }, "study/b.mp3", {} }), QString("Podman 勉強会"));
        QCOMPARE(g.title({ { kw("検証", 10) }, "misc/c.mp3", {} }), QString("検証"));                // no hint -> topic only
        QVERIFY(g.title({ {}, "x.mp3", {} }).isEmpty());                                              // nothing to say
    }
    void secondKeywordNeedsToBeStrongAndDistinct()
    {
        RuleBasedTitleGenerator g;
        QCOMPARE(g.title({ { kw("共有フォルダ", 10), kw("フォルダ", 9.5) }, "x", {} }), QString("共有フォルダ"));   // contained
        QCOMPARE(g.title({ { kw("Docker", 10), kw("会社", 3) }, "x", {} }), QString("Docker"));                    // too weak
        const QString longTitle = g.title({ { kw(QString(40, QChar(0x3042)), 10) }, "x", {} });
        QVERIFY(longTitle.size() <= RuleBasedTitleGenerator::kMaxTitleChars);
        QVERIFY(longTitle.endsWith(QChar(0x2026)));
    }
    void kindFromPathBeatsSpeechAndNeedsRepetition()
    {
        QCOMPARE(RuleBasedTitleGenerator::detectKind("Meetings/2026/x.m4a", {}), QString("会議"));
        QCOMPARE(RuleBasedTitleGenerator::detectKind("a/b.mp3", { "今日の会議では", "次回の会議は" }), QString("会議"));
        QCOMPARE(RuleBasedTitleGenerator::detectKind("a/b.mp3", { "一度だけ会議と言った" }), QString(""));   // one mention is not enough
        QCOMPARE(RuleBasedTitleGenerator::detectKind("podcasts/ep1.mp3", { "会議会議会議" }), QString("ポッドキャスト")); // path first
        QCOMPARE(RuleBasedTitleGenerator::detectKind("voice_memo_0731.m4a", {}), QString("メモ"));
        // keyword already says it: no "会議会議"
        RuleBasedTitleGenerator g;
        QCOMPARE(g.title({ { kw("定例会議", 10) }, "meetings/x.m4a", {} }), QString("定例会議"));
    }

    // ---- storage
    void aiResultsReplaceAiResultsButNeverUserText()
    {
        const qint64 id = addRecording("a.mp3");
        AnalysisRepository repo(db);
        bool kept = true;
        QVERIFY(repo.saveAi(id, AnalysisRepository::Title, "AI 1", "m", &kept));
        QVERIFY(!kept);
        QVERIFY(repo.saveAi(id, AnalysisRepository::Title, "AI 2", "m"));
        AnalysisRepository::Entry e;
        QVERIFY(repo.current(id, AnalysisRepository::Title, &e));
        QCOMPARE(e.content, QString("AI 2"));
        QVERIFY(!e.isUser());
        QVERIFY(repo.saveUser(id, AnalysisRepository::Title, "私のタイトル"));
        QVERIFY(repo.saveAi(id, AnalysisRepository::Title, "AI 3", "m", &kept));
        QVERIFY(kept);                                           // the user's title stays
        QVERIFY(repo.current(id, AnalysisRepository::Title, &e));
        QCOMPARE(e.content, QString("私のタイトル"));
        QVERIFY(e.isUser());
        // kinds are independent
        QVERIFY(repo.saveAi(id, AnalysisRepository::Summary, "要約", "m"));
        QVERIFY(repo.current(id, AnalysisRepository::Summary, &e));
        QCOMPARE(e.content, QString("要約"));
        // reverting drops the user's text and the newest AI text is current again
        QVERIFY(repo.revertToAi(id, AnalysisRepository::Title));
        QVERIFY(repo.current(id, AnalysisRepository::Title, &e));
        QCOMPARE(e.content, QString("AI 2"));
        QVERIFY(!repo.saveAi(9999, AnalysisRepository::Title, "x", "m")); // unknown recording
        // exactly one current row per kind
        QSqlQuery q(db.handle());
        QVERIFY(q.exec(QString("SELECT count(*) FROM analyses WHERE recording_id=%1 AND kind='title' AND is_current=1").arg(id)));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1);
    }

    // ---- knowledge builder
    void buildProducesTitleSummaryKeywordsAndSearchRow()
    {
        const qint64 id = addRecording("meetings/standup.m4a");
        transcribe(id, meeting());
        KnowledgeBuilder b(db, *tok);
        KnowledgeBuilder::Result r;
        QString err;
        QVERIFY2(b.build(id, &r, &err), qPrintable(err));
        QVERIFY(!r.title.isEmpty());
        QVERIFY(r.title.endsWith("会議"));                         // folder name says "meetings"
        QVERIFY2(r.summary.contains("Podman"), qPrintable(r.summary));
        QVERIFY(!r.keywords.isEmpty());

        RecordingListModel m(db);                                   // the card shows all of it
        const QModelIndex i = m.index(0, 0);
        QCOMPARE(i.data(RecordingListModel::TitleRole).toString(), r.title);
        QCOMPARE(i.data(RecordingListModel::SummaryRole).toString(), r.summary);
        QVERIFY(!i.data(RecordingListModel::KeywordsRole).toStringList().isEmpty());

        // title and summary words are searchable at recording level
        QSqlQuery q(db.handle());
        q.prepare("SELECT count(*) FROM fts_recording WHERE fts_recording MATCH ?");
        q.addBindValue("summary:" + tok->wakati("Podman").split(' ').first());
        QVERIFY(q.exec() && q.next());
        QCOMPARE(q.value(0).toInt(), 1);
    }
    void rebuildKeepsTheUsersTitleAndRefreshesTheRest()
    {
        const qint64 id = addRecording("meetings/standup.m4a");
        transcribe(id, meeting());
        KnowledgeBuilder b(db, *tok);
        QVERIFY(b.build(id));
        QVERIFY(AnalysisRepository(db).saveUser(id, AnalysisRepository::Title, "週次スタンドアップ"));
        transcribe(id, meeting() + QStringList{ "追加で、Quadletによる自動起動も試す予定です。" });   // re-analysis
        KnowledgeBuilder::Result r;
        QVERIFY(b.build(id, &r));
        QVERIFY(r.keptUserTitle);
        RecordingListModel m(db);
        QCOMPARE(m.index(0, 0).data(RecordingListModel::TitleRole).toString(), QString("週次スタンドアップ"));
        // and the search row carries the user's title
        QSqlQuery q(db.handle());
        // the query is word-split exactly like the index (スタンドアップ -> "スタンド アップ"), as the search UI will do
        q.prepare("SELECT count(*) FROM fts_recording WHERE fts_recording MATCH ?");
        q.addBindValue("title:\"" + tok->wakati("スタンドアップ") + "\"");
        QVERIFY(q.exec() && q.next());
        QCOMPARE(q.value(0).toInt(), 1);
    }
    void silentRecordingKeepsFileNameTitleAndHasNoSummary()
    {
        const qint64 id = addRecording("memos/silence.wav");
        transcribe(id, {});
        KnowledgeBuilder::Result r;
        QVERIFY(KnowledgeBuilder(db, *tok).build(id, &r));
        QVERIFY(r.title.isEmpty() && r.summary.isEmpty());
        RecordingListModel m(db);
        QCOMPARE(m.index(0, 0).data(RecordingListModel::TitleRole).toString(), QString("silence.wav"));
    }
    void hallucinatedLoopsDoNotReachSummaryOrKeywords()
    {
        const qint64 id = addRecording("a.m4a");
        QStringList segs = { "今日はPodmanへの移行方針について議論します。", "rootlessで動かせるのがメリットです。" };
        for (int i = 0; i < 25; ++i) segs << "担当者の方に、";                       // whisper stuck in a loop
        KnowledgeBuilder::Result r;
        QVERIFY(KnowledgeBuilder(db, *tok).build((transcribe(id, segs), id), &r));
        QVERIFY2(r.summary.count("担当者の方に") <= 1, qPrintable(r.summary));
        for (const Keyword& k : r.keywords)
            if (k.term == "担当者") QVERIFY2(k.count <= 1, qPrintable(QString::number(k.count)));
        QVERIFY(r.title.contains("Podman") || r.title.contains("移行") || r.title.contains("rootless"));
    }
};

QTEST_MAIN(TstSummary)
#include "tst_summary.moc"
