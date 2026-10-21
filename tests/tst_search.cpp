#include <QElapsedTimer>
#include <QSqlError>
#include <QSqlQuery>
#include <QtTest>

#include "core/AnalysisRepository.h"
#include "core/Database.h"
#include "core/ITokenizer.h"
#include "core/KnowledgeBuilder.h"
#include "core/SearchService.h"
#include "core/TextIndexer.h"
#include "core/TranscriptRepository.h"

using namespace ake;
using Src = SearchHit::Source;

class TstSearch : public QObject {
    Q_OBJECT
    Database db;
    std::unique_ptr<ITokenizer> tok;
    std::unique_ptr<SearchService> svc;
    qint64 docker = 0, study = 0, hiring = 0, memo = 0, fresh = 0;

    qint64 addRecording(const QString& relPath, const QStringList& segments, bool build = true)
    {
        QSqlQuery q(db.handle());
        q.prepare("INSERT INTO recordings(folder_id,rel_path,abs_path,status,created_at,updated_at) VALUES(1,?,?,?,?,?)");
        q.addBindValue(relPath); q.addBindValue("/f/" + relPath); q.addBindValue(segments.isEmpty() && !build ? "new" : "done");
        q.addBindValue(1000 + m_n++); q.addBindValue(0);
        if (!q.exec()) qFatal("%s", qPrintable(q.lastError().text()));
        const qint64 id = q.lastInsertId().toLongLong();
        if (build) {
            TranscriptResult r; r.language = "ja"; r.model = "t";
            qint64 t = 0;
            for (const QString& s : segments) { r.segments.append({ t, t + 3000, s }); t += 3000; }
            if (!TranscriptRepository(db).save(id, r)) qFatal("save");
            QString err;
            if (!KnowledgeBuilder(db, *tok).build(id, nullptr, &err)) qFatal("build: %s", qPrintable(err));
        }
        return id;
    }
    int m_n = 0;
    QList<qint64> ids(const SearchResult& r)
    {
        QList<qint64> l;
        for (const auto& h : r.hits) l << h.recordingId;
        return l;
    }
    SearchResult find(const QString& q)
    {
        const SearchResult r = svc->search(q);
        if (!svc->lastError().isEmpty()) qWarning("search error for '%s': %s", qPrintable(q), qPrintable(svc->lastError()));
        return r;
    }
    QString highlighted(const SearchHit& h, int i)
    {
        if (h.highlights.size() < 2 * (i + 1)) return QStringLiteral("<no highlight>");
        return h.snippet.mid(h.highlights[2 * i], h.highlights[2 * i + 1]);
    }

private slots:
    void init()
    {
        tok = createTokenizer();
        QVERIFY(db.open(":memory:"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("INSERT INTO folders(id,path) VALUES(1,'/f')"));
        m_n = 0;
        docker = addRecording("meetings/docker.m4a", {
            "今日はDocker ComposeからPodmanへの移行方針について議論します。",
            "Podmanはrootlessで動くので、セキュリティ面のメリットが大きいです。",
            "共有フォルダの権限は注意が必要です。",
            "移行は段階的に進めます。まずCI環境です。",
            "来週までに検証担当者を決めます。" });
        study = addRecording("study/podman_study.mp3", {
            "Podmanの勉強会を始めます。",
            "rootlessコンテナの仕組みと制約を説明します。",
            "systemdとQuadletで自動起動できます。",
            "最後にPodmanの質問を受け付けます。" });
        hiring = addRecording("interviews/hiring.flac", {
            "C++とSQLiteの経験について伺います。",
            "設計判断の理由を説明してください。" });
        memo = addRecording("memos/shopping.wav", { "牛乳とトマトを買って帰ります。" });
        fresh = addRecording("unanalyzed/interview_04.mp3", {}, false);   // no transcript yet
        svc = std::make_unique<SearchService>(db, *tok);
    }

    // ---- query parsing
    void parsesTermsAndQuotedPhrases()
    {
        QCOMPARE(SearchService::parseTerms("podman 移行"), QStringList({ "podman", "移行" }));
        QCOMPARE(SearchService::parseTerms("  a　b\tc \n"), QStringList({ "a", "b", "c" }));       // full-width space, tab
        QCOMPARE(SearchService::parseTerms("\"共有 フォルダ\" 権限"), QStringList({ "共有 フォルダ", "権限" }));
        QCOMPARE(SearchService::parseTerms("“curly quotes” x"), QStringList({ "curly quotes", "x" }));
        QCOMPARE(SearchService::parseTerms("abc\"unclosed phrase"), QStringList({ "abc", "unclosed phrase" }));
        QVERIFY(SearchService::parseTerms("").isEmpty());
        QVERIFY(SearchService::parseTerms("  \"\"  ").isEmpty());
    }

    // ---- matching
    void singleTermFindsTranscriptHitsAndOtherFields()
    {
        const SearchResult r = find("Podman");
        const QList<qint64> got = ids(r);
        QCOMPARE(QSet<qint64>(got.begin(), got.end()), QSet<qint64>({ docker, study }));
        QVERIFY(r.totalSegmentHits >= 3);
        QVERIFY(find("牛乳").hits.size() == 1 && find("牛乳").hits[0].recordingId == memo);
        QVERIFY(find("kubernetes").isEmpty());
        QVERIFY(find("").isEmpty());
        QVERIFY(find("   ").isEmpty());
    }
    void termsAreAndedAcrossTheWholeRecordingNotOneSentence()
    {
        // 検証担当者 (segment 5) and 共有フォルダ (segment 3) are in different sentences of the same recording
        QCOMPARE(ids(find("共有フォルダ 検証担当者")), QList<qint64>({ docker }));
        QCOMPARE(ids(find("Podman 牛乳")), QList<qint64>());                 // in different recordings: no match
        QCOMPARE(ids(find("rootless Quadlet")), QList<qint64>({ study }));
    }
    void quotedPhrasesNeedAdjacencyAndOrder()
    {
        QCOMPARE(ids(find("\"共有フォルダ\"")), QList<qint64>({ docker }));
        QCOMPARE(ids(find("\"フォルダ共有\"")), QList<qint64>());             // reversed
        QCOMPARE(ids(find("\"Docker Compose\"")), QList<qint64>({ docker }));
        QCOMPARE(ids(find("\"Compose Docker\"")), QList<qint64>());
    }
    void fullWidthCaseAndPrefixAreHandled()
    {
        QCOMPARE(ids(find("ＰＯＤＭＡＮ")).size(), 2);                  // full-width capitals
        QCOMPARE(ids(find("podman")).size(), 2);
        QCOMPARE(ids(find("podm")).size(), 2);                                  // prefix of the last term
        QCOMPARE(ids(find("pod")).size(), 2);
        QCOMPARE(ids(find("rootless compos")), QList<qint64>({ docker }));      // prefix of the last term: Compose
        QCOMPARE(ids(find("compos rootless")).size(), 0);                       // ... but not of earlier terms
        QCOMPARE(ids(find("共有フォ")), QList<qint64>({ docker }));              // Japanese prefix of a token
    }
    void fileNamesAreSearchableEvenBeforeAnalysis()
    {
        const SearchResult r = find("interview_04");
        QCOMPARE(ids(r), QList<qint64>({ fresh }));
        QCOMPARE(r.hits[0].source, Src::FileName);
        QVERIFY(highlighted(r.hits[0], 0).toLower() == "interview_04");
        QCOMPARE(ids(find("unanalyzed")), QList<qint64>({ fresh }));
        QCOMPARE(ids(find("docker.m4a")), QList<qint64>({ docker }));
    }
    void specialCharactersNeverBreakTheQuery()
    {
        for (const char* nasty : { "\"", "\"\"", "*", "(", ")", "OR", "AND", "NOT", "NEAR(a b)", "-x", "a OR b", "%", "_", "\\",
                                   "c++", "foo*", "'; DROP TABLE recordings; --", "col:val", "^start", "{x}", "a\"b" }) {
            const SearchResult r = find(QString::fromUtf8(nasty));
            QVERIFY2(svc->lastError().isEmpty(), nasty);
            Q_UNUSED(r);
        }
        QVERIFY(find("%").isEmpty());                       // LIKE wildcards are literal
        QVERIFY(find("_").isEmpty());
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("SELECT count(*) FROM recordings"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 5);                    // nothing was dropped
    }
    void searchesCorrectedTextNotTheRecognizedOne()
    {
        QSqlQuery q(db.handle());
        QVERIFY(q.exec(QString("UPDATE segments SET edited_text='牛乳とバナナを買います。' WHERE transcript_id IN (SELECT id FROM transcripts WHERE recording_id=%1)").arg(memo)));
        // what the transcript editor will do after saving a correction: rebuild everything derived from the text
        QVERIFY(KnowledgeBuilder(db, *tok).build(memo));
        QCOMPARE(ids(find("バナナ")), QList<qint64>({ memo }));
        QVERIFY(find("トマト").isEmpty());                    // also gone from the regenerated summary and keywords
    }
    void oldTranscriptGenerationsDoNotMatch()
    {
        TranscriptResult r; r.language = "ja"; r.model = "t";
        r.segments.append({ 0, 3000, "新しい内容はKubernetesです。" });
        QVERIFY(TranscriptRepository(db).save(memo, r));
        QVERIFY(KnowledgeBuilder(db, *tok).build(memo));
        QVERIFY(find("牛乳").isEmpty());
        QCOMPARE(ids(find("kubernetes")), QList<qint64>({ memo }));
    }

    // ---- ranking
    void titleMatchesOutrankBodyMatches()
    {
        // zebratool is mentioned once among many more frequent topics, so it is NOT among the generated keywords/title
        QStringList body = { "今日はzebratoolの使い方について少し触れました。" };
        for (int i = 0; i < 3; ++i)
            body << "予算会議の資料と営業報告の数字を確認しました。" << "採用計画と人事制度の見直しを議論しました。" << "顧客対応の品質と物流の改善を検討しました。"
                 << "在庫管理と購買契約の条件を整理しました。" << "広報戦略と展示会の準備を進めました。";
        const qint64 bodyOnly = addRecording("misc/body.mp3", body);
        const qint64 titled = addRecording("misc/titled.mp3", { "関係のない雑談をしています。天気の話です。" });
        QVERIFY(AnalysisRepository(db).saveUser(titled, AnalysisRepository::Title, "zebratool入門"));
        QVERIFY(TextIndexer(db, *tok).rebuildRecordingFts(titled));
        const SearchResult r = find("zebratool");
        QCOMPARE(r.hits.size(), 2);
        QCOMPARE(r.hits[0].recordingId, titled);
        QCOMPARE(r.hits[0].source, Src::Title);
        QCOMPARE(r.hits[1].recordingId, bodyOnly);
        QVERIFY(r.hits[0].score > r.hits[1].score);
    }
    void moreEvidenceRanksHigher()
    {
        const SearchResult r = find("Podman");
        QCOMPARE(r.hits.size(), 2);
        QVERIFY(r.hits[0].score > r.hits[1].score);
    }

    // ---- snippets
    void snippetsAreAroundTheMatchAndHighlightsAreExact()
    {
        const SearchResult r = find("rootless");
        for (const SearchHit& h : r.hits) {
            QVERIFY(!h.snippet.isEmpty());
            QCOMPARE(h.source, Src::Transcript);
            QVERIFY(h.snippetMs >= 0);
            QVERIFY(h.highlights.size() >= 2);
            QCOMPARE(highlighted(h, 0).toLower(), QString("rootless"));
            for (int i = 0; i + 1 < h.highlights.size(); i += 2) {        // sane, ordered, in bounds
                QVERIFY(h.highlights[i] >= 0 && h.highlights[i + 1] > 0);
                QVERIFY(h.highlights[i] + h.highlights[i + 1] <= h.snippet.size());
                if (i >= 2) QVERIFY(h.highlights[i] >= h.highlights[i - 2] + h.highlights[i - 1]);
            }
        }
        // the snippet comes from the right moment of the audio
        const SearchResult s = find("Quadlet");
        QCOMPARE(s.hits[0].snippetMs, qint64(6000));                      // 3rd segment of the study recording
    }
    void theSnippetShowsTheSegmentsAroundTheMatch()
    {
        // whisper segments are ~5 s: the matching one alone cannot tell whether this is the recording wanted
        const qint64 id = addRecording("misc/ctx.mp3", { "前の発言ではスケジュールの話をしました。", "ターゲット語についての話です。", "次の発言で結論を述べます。" });
        const SearchResult r = find("ターゲット語");
        QCOMPARE(r.hits[0].recordingId, id);
        const SearchHit& h = r.hits[0];
        QVERIFY2(h.snippet.contains("スケジュール") && h.snippet.contains("結論"), qPrintable(h.snippet));   // both neighbours
        QCOMPARE(h.snippetMs, qint64(3000));                                                              // the time is the matching segment's
        QVERIFY(h.highlights.size() >= 2);
        QCOMPARE(h.snippet.mid(h.highlights[0], h.highlights[1]), QString("ターゲット語"));
        QVERIFY2(h.snippet.size() > QString("ターゲット語についての話です。").size() + 10, "more than the segment alone");
        QVERIFY2(h.snippet.size() <= SearchService::kSnippetBefore + SearchService::kSnippetAfter + 6 + 2, qPrintable(QString::number(h.snippet.size())));
    }
    void snippetsAtTheEdgesOfATranscriptAndWithLatinNeighbours()
    {
        const qint64 first = addRecording("misc/first.mp3", { "ターゲット語から始まります。", "二番目の発言です。" });
        const qint64 last = addRecording("misc/last.mp3", { "最初の発言です。", "最後にターゲット語です。" });
        const SearchResult r = find("ターゲット語");
        for (const SearchHit& h : r.hits) {
            if (h.recordingId == first) { QVERIFY(h.snippet.contains("二番目")); QVERIFY(!h.snippet.startsWith(QChar(0x2026))); }
            if (h.recordingId == last) { QVERIFY(h.snippet.contains("最初の発言")); QVERIFY(!h.snippet.endsWith(QChar(0x2026))); }
        }
        addRecording("misc/en.mp3", { "ask not what your", "country can do for you", "ask what you can do" });
        const SearchResult en = find("country");
        QVERIFY2(en.hits[0].snippet.contains("your country can do for you ask what"), qPrintable(en.hits[0].snippet));   // spaces between Latin words
    }
    void shortSegmentsAreAddedUntilTheSnippetWindowIsFull()
    {
        QStringList segs;
        for (int i = 0; i < 20; ++i) segs << QString("断片%1です").arg(i, 2, 10, QChar('0'));    // 6-character segments, as small/turbo print them
        segs[10] = "ターゲット語";
        const qint64 id = addRecording("misc/short.mp3", segs);
        const SearchResult r = find("ターゲット語");
        QCOMPARE(r.hits[0].recordingId, id);
        const QString sn = r.hits[0].snippet;
        QVERIFY2(sn.contains("断片09です") && sn.contains("断片07です") && sn.contains("断片13です"), qPrintable(sn));   // several on each side
        QVERIFY2(!sn.contains("断片00です") && !sn.contains("断片19です"), qPrintable(sn));                              // but not the whole transcript
        QVERIFY2(sn.size() <= SearchService::kSnippetBefore + SearchService::kSnippetAfter + 6 + 2, qPrintable(QString::number(sn.size())));
        QCOMPARE(r.hits[0].snippetMs, qint64(30000));
    }
    // ---- by sound: a word the recogniser wrote as a similar-sounding other word (design.md "読みの近い語を拾う検索")
    void nothingBySpellingFindsCandidatesWhoseReadingIsClose()
    {
        if (!tok->hasReadings()) QSKIP("needs a tokenizer with readings (MeCab)");
        const qint64 near = addRecording("sound/near.mp3", { "前の発言です。", "配当の段階に進みます。", "次の発言です。" });   // はいとう ~ かいとう
        addRecording("sound/far.mp3", { "牛乳を買います。" });
        const SearchResult r = find("回答");
        QVERIFY(r.similar);
        QCOMPARE(r.hits.size(), 1);
        const SearchHit& h = r.hits[0];
        QCOMPARE(h.recordingId, near);
        QVERIFY(h.similar);
        QCOMPARE(h.distance, 1);
        QCOMPARE(h.snippetMs, qint64(3000));                                  // the segment that sounds like it
        QVERIFY2(h.snippet.contains("配当") && h.snippet.contains("前の発言") && h.snippet.contains("次の発言"), qPrintable(h.snippet));
        QVERIFY(h.highlights.isEmpty());                                      // a sound match has no position in the spelling
        QVERIFY(svc->lastError().isEmpty());
    }
    void spellingAlwaysWinsAndTheSoundSearchOnlyRunsWhenNothingMatched()
    {
        addRecording("sound/exact.mp3", { "回答をお待ちしています。" });
        addRecording("sound/near.mp3", { "配当の段階に進みます。" });
        const SearchResult r = find("回答");
        QVERIFY(!r.similar);                                                  // found by spelling: the sound candidates are not mixed in
        QCOMPARE(r.hits.size(), 1);
        QVERIFY(!r.hits[0].similar);
        QVERIFY(!r.hits[0].highlights.isEmpty());
    }
    void homophonesAreAddedAfterTheSpellingHitsEvenWhenSomethingMatchedBySpelling()
    {
        if (!tok->hasReadings()) QSKIP("needs a tokenizer with readings (MeCab)");
        const qint64 exact = addRecording("sound/exact.mp3", { "システム障害が発生しました。" });
        const qint64 homo = addRecording("sound/homophone.mp3", { "システム症外が発生しました。" });         // しょうがい, as the recogniser wrote it
        addRecording("sound/other.mp3", { "牛乳を買います。" });
        const SearchResult r = find("障害");
        QVERIFY(!r.similar);                                                    // the result has spelling hits: not a "nothing found" result
        QCOMPARE(ids(r), QList<qint64>({ exact, homo }));                        // spelling first, then the homophone
        QVERIFY(!r.hits[0].similar);
        QVERIFY(r.hits[1].similar);
        QCOMPARE(r.hits[1].distance, 0);
        QVERIFY(r.hits[1].snippet.contains("症外"));
        // a reading that merely resembles the typed word is not added next to spelling hits (that is for "nothing found")
        addRecording("sound/near.mp3", { "システム小外が発生しました。" });                                  // しょうがい too: identical reading, so it is added
        QCOMPARE(find("障害").hits.size(), 3);
        addRecording("sound/similar.mp3", { "システム傷害罪です。" });                                       // しょうがいざい: contains the reading
        QVERIFY(find("障害").hits.size() >= 3);
        // two kana: too many homophones to add them
        const qint64 rain = addRecording("sound/rain.mp3", { "今日は雨が降ります。" });
        addRecording("sound/candy.mp3", { "飴を舐めます。" });                                                // あめ
        QCOMPARE(ids(find("雨")), QList<qint64>({ rain }));
    }
    void closerSoundsComeFirstAndShortWordsAreNotMatched()
    {
        if (!tok->hasReadings()) QSKIP("needs a tokenizer with readings (MeCab)");
        const qint64 same = addRecording("sound/same.mp3", { "快刀乱麻を断つ。" });                // かいとう: other kanji, same sound
        const qint64 near = addRecording("sound/near.mp3", { "配当の段階に進みます。" });
        const SearchResult r = find("回答");
        QCOMPARE(ids(r), QList<qint64>({ same, near }));
        QCOMPARE(r.hits[0].distance, 0);
        QCOMPARE(r.hits[1].distance, 1);
        addRecording("sound/phone.mp3", { "電話の件です。" });                                            // でんわ
        QVERIFY(find("天和").hits.isEmpty());                                   // てんわ: 3 kana, one off, still too short to compare by sound
        QVERIFY(!find("犬").similar && find("犬").hits.isEmpty());              // いぬ: 2 kana
    }
    void everyTermMustBeSatisfiedAndLatinWordsAreNeverMatchedBySound()
    {
        if (!tok->hasReadings()) QSKIP("needs a tokenizer with readings (MeCab)");
        const qint64 both = addRecording("sound/both.mp3", { "配当の段階に進みます。", "Podmanを使います。" });
        addRecording("sound/onlySound.mp3", { "配当の段階に進みます。" });
        const SearchResult r = find("回答 Podman");                            // one term by sound, one exact
        QVERIFY(r.similar);
        QCOMPARE(ids(r), QList<qint64>({ both }));
        QVERIFY(find("回答 zzzzqqq").hits.isEmpty());                          // a term that is nowhere
        QVERIFY(find("Podmn").hits.isEmpty());                                 // a Latin typo is not repaired
    }
    void thePhoneticFormsOfOlderLibrariesAreFilledInOnTheFirstSoundSearch()
    {
        if (!tok->hasReadings()) QSKIP("needs a tokenizer with readings (MeCab)");
        const qint64 near = addRecording("sound/old.mp3", { "配当の段階に進みます。" });
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("UPDATE segments SET phonetic=''"));                    // a library indexed before the column existed
        const SearchResult r = find("回答");
        QCOMPARE(ids(r), QList<qint64>({ near }));
        QVERIFY(q.exec("SELECT count(*) FROM segments WHERE phonetic=''"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 0);
    }
    void theUsersCorrectionDecidesWhatSoundsLike()
    {
        if (!tok->hasReadings()) QSKIP("needs a tokenizer with readings (MeCab)");
        const qint64 id = addRecording("sound/edit.mp3", { "牛乳を買います。" });
        QCOMPARE(find("回答").hits.size(), 0);
        QSqlQuery q(db.handle());
        QVERIFY(q.exec(QString("UPDATE segments SET edited_text='配当の段階です。' WHERE transcript_id=(SELECT id FROM transcripts WHERE recording_id=%1)").arg(id)));
        QVERIFY(TextIndexer(db, *tok).index(id));                              // what saving a correction does
        QCOMPARE(ids(find("回答")), QList<qint64>({ id }));
    }
    void withoutReadingsTheFallbackDoesNothing()
    {
        if (tok->hasReadings()) QSKIP("this is about the n-gram fallback");
        addRecording("sound/near.mp3", { "配当の段階に進みます。" });
        const SearchResult r = find("回答");
        QVERIFY(!r.similar);
        QVERIFY(r.hits.isEmpty());
    }
    void theSnippetLeavesOutWhisperSilenceTags()
    {
        addRecording("misc/tags.mp3", { "[音声なし]", "前の発言です。", "ターゲット語の話です。[音楽]", "[音声なし]", "次の発言です。" });
        const SearchResult r = find("ターゲット語");
        QVERIFY2(!r.hits[0].snippet.contains("音声なし") && !r.hits[0].snippet.contains("音楽"), qPrintable(r.hits[0].snippet));
        QVERIFY(r.hits[0].snippet.contains("前の発言") && r.hits[0].snippet.contains("次の発言"));
        QVERIFY(r.hits[0].highlights.size() >= 2);
        QCOMPARE(r.hits[0].snippet.mid(r.hits[0].highlights[0], r.hits[0].highlights[1]), QString("ターゲット語"));
    }
    void theSnippetUsesTheUsersCorrectionOfANeighbour()
    {
        const qint64 id = addRecording("misc/edit.mp3", { "誤った前の発言です。", "ターゲット語の話です。", "次の発言です。" });
        QSqlQuery q(db.handle());
        QVERIFY(q.exec(QString("UPDATE segments SET edited_text='訂正された前の発言です。' WHERE idx=0 AND transcript_id="
                               "(SELECT id FROM transcripts WHERE recording_id=%1 AND is_current=1)").arg(id)));
        const SearchResult r = find("ターゲット語");
        QVERIFY2(r.hits[0].snippet.contains("訂正された前"), qPrintable(r.hits[0].snippet));
    }
    void snippetHighlightsAnOverlappingNgramRunWhole()
    {
        // whatever the tokenizer, the highlighted part is exactly what was typed (n-grams overlap: ターゲット -> ター ーゲ ゲッ ット)
        const qint64 id = addRecording("misc/kata.mp3", { "前置きのあとでターゲットについて話します。" });
        const SearchResult r = find("ターゲット");
        QCOMPARE(r.hits[0].recordingId, id);
        const SearchHit& h = r.hits[0];
        QVERIFY2(h.highlights.size() >= 2, "no highlight found");
        QCOMPARE(h.snippet.mid(h.highlights[0], h.highlights[1]), QString("ターゲット"));
    }
    void longSegmentsAreCutWithEllipses()
    {
        const QString filler(120, QChar(0x3042));                          // あ x120
        const qint64 id = addRecording("misc/long.mp3", { filler + "ターゲット語" + filler + "。" });
        const SearchResult r = find("ターゲット語");
        QCOMPARE(r.hits[0].recordingId, id);
        const QString s = r.hits[0].snippet;
        QVERIFY2(s.startsWith(QChar(0x2026)) && s.endsWith(QChar(0x2026)), qPrintable(QString::number(s.size()) + ": " + s.left(30) + " ... " + s.right(30)));
        QVERIFY2(s.size() <= SearchService::kSnippetBefore + SearchService::kSnippetAfter + 6 + 2, qPrintable(QString::number(s.size())));
        QCOMPARE(highlighted(r.hits[0], 0), QString("ターゲット語"));
        // a short segment is not cut
        const SearchResult m = find("牛乳");
        QVERIFY(!m.hits[0].snippet.startsWith(QChar(0x2026)) && !m.hits[0].snippet.endsWith(QChar(0x2026)));
    }
    void multiTermSnippetPrefersASegmentWithSeveralTermsAndHighlightsAll()
    {
        const SearchResult r = find("Docker Podman");
        QCOMPARE(r.hits[0].recordingId, docker);
        QVERIFY(r.hits[0].snippet.contains("Docker") && r.hits[0].snippet.contains("Podman"));
        QVERIFY(r.hits[0].highlights.size() >= 4);
        QCOMPARE(highlighted(r.hits[0], 0), QString("Docker"));
    }
    void summaryOnlyAndTitleOnlyMatchesAreShown()
    {
        AnalysisRepository repo(db);
        QVERIFY(repo.saveUser(memo, AnalysisRepository::Summary, "家族の夕食の買い物リストを確認した。"));
        QVERIFY(TextIndexer(db, *tok).rebuildRecordingFts(memo));
        const SearchResult s = find("夕食");
        QCOMPARE(ids(s), QList<qint64>({ memo }));
        QCOMPARE(s.hits[0].source, Src::Summary);
        QCOMPARE(highlighted(s.hits[0], 0), QString("夕食"));
        QCOMPARE(s.hits[0].segmentHits, 0);
        QCOMPARE(s.hits[0].snippetMs, qint64(-1));
        QVERIFY(repo.saveUser(memo, AnalysisRepository::Title, "週末の買い出し"));
        QVERIFY(TextIndexer(db, *tok).rebuildRecordingFts(memo));
        const SearchResult t = find("買い出し");
        QCOMPARE(t.hits[0].source, Src::Title);
    }
    void limitCapsTheResultList()
    {
        for (int i = 0; i < 12; ++i) addRecording(QString("bulk/%1.mp3").arg(i), { "共通のキーワードを含む発言です。" });
        QCOMPARE(svc->search("共通のキーワード", 5).hits.size(), 5);
        QCOMPARE(svc->search("共通のキーワード").hits.size(), 12);
    }

    // ---- suggestions
    void suggestsTagsRecordingsAndSegments()
    {
        const Suggestions s = svc->suggest("podm");
        QVERIFY2(svc->lastError().isEmpty(), qPrintable(svc->lastError()));
        QVERIFY(!s.tags.isEmpty());
        QCOMPARE(s.tags[0].name.toLower(), QString("podman"));
        QCOMPARE(s.tags[0].count, 2);                                       // two recordings carry it
        QVERIFY(s.recordings.isEmpty() == false);                           // study/podman_study.mp3 by file name
        QVERIFY(!s.segments.isEmpty());
        for (const auto& seg : s.segments) { QVERIFY(!seg.snippet.isEmpty()); QVERIFY(seg.startMs >= 0); QVERIFY(!seg.recordingTitle.isEmpty()); }
        QVERIFY(svc->suggest("").isEmpty());
        QVERIFY(svc->suggest("zzzzzz").isEmpty());
        QVERIFY(svc->suggest("podm", 1).tags.size() <= 1 && svc->suggest("podm", 1).segments.size() <= 1);
    }
    void suggestsRecordingsByTitleAndFileName()
    {
        QVERIFY(AnalysisRepository(db).saveUser(memo, AnalysisRepository::Title, "週末の買い出しメモ"));
        const Suggestions s = svc->suggest("買い出し");
        QCOMPARE(s.recordings.size(), 1);
        QCOMPARE(s.recordings[0].id, memo);
        QCOMPARE(s.recordings[0].title, QString("週末の買い出しメモ"));
        const Suggestions f = svc->suggest("interview_04");
        QCOMPARE(f.recordings.size(), 1);
        QCOMPARE(f.recordings[0].id, fresh);
        QCOMPARE(f.recordings[0].title, QString("unanalyzed/interview_04.mp3"));   // no title yet: the path
    }

    // ---- scale
    void staysFastOnALargerLibrary()
    {
        QSqlQuery q(db.handle());
        for (int r = 0; r < 120; ++r) {
            QStringList segs;
            for (int s = 0; s < 60; ++s) segs << QString("話題%1について共有フォルダの権限とPodmanの運用%2を議論しました。").arg((r + s) % 30).arg(s);
            if (r % 40 == 0) segs << "固有のキーワードUniqueTermXYZを含みます。";
            addRecording(QString("bulk/r%1.mp3").arg(r), segs);
        }
        QElapsedTimer t; t.start();
        const SearchResult a = find("共有フォルダ 権限");
        const qint64 common = t.elapsed();
        QVERIFY2(a.hits.size() >= 120, qPrintable(QString::number(a.hits.size())));
        t.restart();
        const SearchResult b = find("UniqueTermXYZ");
        const qint64 rare = t.elapsed();
        QCOMPARE(b.hits.size(), 3);
        t.restart();
        svc->suggest("podm");
        const qint64 sug = t.elapsed();
        QVERIFY2(common < 800 && rare < 300 && sug < 800, qPrintable(QString("common %1 ms, rare %2 ms, suggest %3 ms").arg(common).arg(rare).arg(sug)));
        qInfo("library of %d segments: common %lld ms, rare %lld ms, suggest %lld ms",
              int(7200 + 3 + 3 + 2 + 1 + 5), common, rare, sug);
    }
};

QTEST_MAIN(TstSearch)
#include "tst_search.moc"
