#include <QThread>
#include <QtTest>
#include <thread>

#include "core/ITokenizer.h"
#include "core/KeywordExtractor.h"
#include "core/NgramTokenizer.h"

using namespace ake;
using K = Token::Kind;

namespace {
Token N(const QString& s, bool weak = false) { return Token{ s, s, K::Noun, weak }; }
Token O(const QString& s) { return Token{ s, s, K::Other, false }; }
Token P(const QString& s = "。") { return Token{ s, s, K::Punct, false }; }

QStringList terms(const QList<Keyword>& k)
{
    QStringList l;
    for (const auto& x : k) l << x.term;
    return l;
}
QSet<QString> setOf(const QList<Keyword>& k)
{
    const QStringList l = terms(k);
    return QSet<QString>(l.begin(), l.end());
}
QList<Token> repeat(const QList<Token>& t, int n)
{
    QList<Token> out;
    for (int i = 0; i < n; ++i) out += t + QList<Token>{ KeywordExtractor::sentenceBreak() };
    return out;
}
} // namespace

class TstTokenizer : public QObject {
    Q_OBJECT
    std::unique_ptr<ITokenizer> mecab;

    QStringList surfaces(const QList<Token>& t)
    {
        QStringList l;
        for (const auto& x : t) l << x.surface;
        return l;
    }
    Token find(const QList<Token>& t, const QString& surface)
    {
        for (const auto& x : t) if (x.surface == surface) return x;
        return {};
    }

private slots:
    void initTestCase()
    {
        QString note;
        mecab = createTokenizer(&note);
        if (mecab->name() != "mecab") qWarning("MeCab not available: %s", qPrintable(note));
    }

    // ---- MeCab
    void mecabClassifiesTokens()
    {
        if (mecab->name() != "mecab") QSKIP("MeCab not available");
        const auto t = mecab->tokenize("共有フォルダを来月までに確認する場合、第3回の会議で。");
        QCOMPARE(find(t, "共有").kind, K::Noun);
        QVERIFY(!find(t, "共有").weak);
        QCOMPARE(find(t, "フォルダ").kind, K::Noun);
        QCOMPARE(find(t, "を").kind, K::Other);
        QVERIFY2(find(t, "来月").weak, "時相名詞 is not keyword material");
        QVERIFY2(find(t, "場合").weak, "副詞的名詞 is not keyword material");
        QCOMPARE(find(t, "する").kind, K::Verb);
        QCOMPARE(find(t, "する").base, QString("する"));
        QCOMPARE(find(t, "3").kind, K::Number);
        QCOMPARE(find(t, "、").kind, K::Punct);
        QCOMPARE(find(t, "。").kind, K::Punct);
        QCOMPARE(find(t, "会議").kind, K::Noun);
    }
    void mecabMarksNounSuffixesButNotHonorifics()
    {
        if (mecab->name() != "mecab") QSKIP("MeCab not available");
        const auto t = mecab->tokenize("担当者に社内の田中さんと開発者たちを紹介");
        QCOMPARE(find(t, "者").kind, K::Suffix);
        QCOMPARE(find(t, "内").kind, K::Suffix);
        QCOMPARE(find(t, "さん").kind, K::Other);   // honorific: not part of a keyword
        QCOMPARE(find(t, "たち").kind, K::Other);   // plural marker
    }
    void mecabKeepsLatinTechnicalWordsAsNouns()
    {
        if (mecab->name() != "mecab") QSKIP("MeCab not available");
        const auto t = mecab->tokenize("DockerからPodmanへ、rootless運用とsystemd連携。");
        for (const char* w : { "Docker", "Podman", "rootless", "systemd" }) QCOMPARE(find(t, w).kind, K::Noun);
        QCOMPARE(find(t, "運用").kind, K::Noun);
    }
    void fullWidthIsNormalizedLikeAscii()
    {
        QCOMPARE(mecab->wakati("ＧＰＴ４の利用ｶﾞｲﾄﾞ"), mecab->wakati("GPT4の利用ガイド"));
        QCOMPARE(ITokenizer::normalize("ＡＢＣ１２３"), QString("ABC123"));
        QCOMPARE(ITokenizer::normalize("ｶﾞ"), QString("ガ"));
    }
    void wakatiDropsPunctuationAndJoinsWithSpaces()
    {
        const QString w = mecab->wakati("今日は、Dockerの話です。");
        QVERIFY2(!w.contains("、") && !w.contains("。"), qPrintable(w));
        QVERIFY(w.contains("Docker"));
        QVERIFY(!w.contains("  "));
        QVERIFY(!w.startsWith(' ') && !w.endsWith(' '));
        // every wakati word is a substring of the (normalized) input
        for (const QString& part : w.split(' ')) QVERIFY2(ITokenizer::normalize("今日は、Dockerの話です。").contains(part), qPrintable(part));
    }
    void emptyAndWhitespaceInput()
    {
        QVERIFY(mecab->tokenize("").isEmpty());
        QVERIFY(mecab->tokenize("   \n\t ").isEmpty());
        QCOMPARE(mecab->wakati(""), QString());
    }
    void longTextIsFast()
    {
        QString text;
        for (int i = 0; i < 4000; ++i) text += "Podmanへの移行は段階的に進めます。共有フォルダの権限も確認しました。";
        QElapsedTimer t; t.start();
        const auto toks = mecab->tokenize(text);
        QVERIFY2(t.elapsed() < 3000, qPrintable(QString::number(t.elapsed()) + " ms"));
        QVERIFY(toks.size() > 50000);
    }
    void concurrentUseGivesIdenticalResults()
    {
        const QString text = "Docker ComposeからPodmanへの移行について議論しました。共有フォルダの権限も確認します。";
        const QString expected = mecab->wakati(text);
        std::atomic<int> bad{ 0 };
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i)
            threads.emplace_back([&] { for (int k = 0; k < 200; ++k) if (mecab->wakati(text) != expected) bad++; });
        for (auto& th : threads) th.join();
        QCOMPARE(bad.load(), 0);
    }

    // ---- fallback tokenizer
    void ngramFallbackSplitsByScript()
    {
        NgramTokenizer ng;
        const auto t = ng.tokenize("Docker共有フォルダを3GB確認。");
        QCOMPARE(surfaces(t), QStringList({ "Docker", "共有", "フォ", "ォル", "ルダ", "を", "3GB", "確認", "。" }));
        QCOMPARE(find(t, "Docker").kind, K::Noun);
        QCOMPARE(find(t, "を").kind, K::Other);   // hiragana is not content
        QCOMPARE(find(t, "。").kind, K::Punct);
        QCOMPARE(ng.wakati("共有フォルダ。"), QString("共有 フォ ォル ルダ"));
        QCOMPARE(find(ng.tokenize("3 4"), "3").kind, K::Number);
        QVERIFY(ng.tokenize("").isEmpty());
        QCOMPARE(ng.tokenize("会").first().weak, true); // lone character: not keyword material
    }
    void ngramKeywordsAreWholeWordsNotGluedOverlaps()
    {
        // フォ + ォル + ルダ overlap in the text: the keyword is フォルダ, not フォォルルダ
        NgramTokenizer ng;
        const auto kws = KeywordExtractor::extract(ng.tokenize("共有フォルダの権限管理。検証担当者と議論。"), {}, 8);
        const QStringList t = terms(kws);
        QVERIFY2(t.contains("共有フォルダ"), qPrintable(t.join(",")));
        QVERIFY2(t.contains("権限管理"), qPrintable(t.join(",")));
        QVERIFY2(t.contains("検証担当者"), qPrintable(t.join(",")));
        for (const QString& k : t)                                          // nothing may contain a doubled character that the text lacks
            for (int i = 0; i + 1 < k.size(); ++i)
                if (k[i] == k[i + 1]) QVERIFY2(QString("共有フォルダの権限管理。検証担当者と議論。").contains(QString(2, k[i])), qPrintable(k));
        QCOMPARE(ng.tokenize("フォルダ").at(0).pos, 0);                       // n-gram tokens know where they are
        QCOMPARE(ng.tokenize("フォルダ").at(2).pos, 2);
        QCOMPARE(find(ng.tokenize("Docker"), "Docker").pos, -1);            // whole words never overlap
    }
    void factoryAlwaysReturnsAWorkingTokenizer()
    {
        auto t = createTokenizer();
        QVERIFY(t);
        QVERIFY(!t->wakati("移行の議論").isEmpty());
    }

    // ---- keyword extraction (synthetic tokens: independent of MeCab)
    void nounRunsBecomeCompounds()
    {
        const auto k = KeywordExtractor::extract({ N("共有"), N("フォルダ"), O("を"), N("確認") }, {});
        QVERIFY(terms(k).contains("共有フォルダ"));
        QVERIFY(terms(k).contains("確認"));
        QVERIFY(!terms(k).contains("共有"));
    }
    void nounSuffixesExtendARunButNeverStartOne()
    {
        auto S = [](const QString& x) { return Token{ x, x, K::Suffix, false }; };
        QCOMPARE(terms(KeywordExtractor::extract({ N("担当"), S("者"), O("に") }, {})), QStringList({ "担当者" }));
        QCOMPARE(terms(KeywordExtractor::extract({ N("開発"), N("チーム"), S("内") }, {})), QStringList({ "開発チーム内" }));
        QVERIFY(KeywordExtractor::extract({ S("者"), O("が") }, {}).isEmpty());          // nothing to attach to
        QCOMPARE(terms(KeywordExtractor::extract({ N("会議"), P(), S("内") }, {})), QStringList({ "会議" })); // break in between
    }
    void pronounsAreStopwords()
    {
        QVERIFY(terms(KeywordExtractor::extract(repeat({ N("我々") }, 5) + repeat({ N("Podman") }, 1), {})) == QStringList({ "Podman" }));
    }
    void latinCompoundsKeepTheirSpace()
    {
        const auto k = KeywordExtractor::extract({ N("Docker"), N("Compose"), O("から") }, {});
        QCOMPARE(k.size(), 1);
        QCOMPARE(k[0].term, QString("Docker Compose"));
        QCOMPARE(k[0].wakati, QString("Docker Compose")); // phrase-queryable in FTS
        QCOMPARE(k[0].key, QString("docker compose"));
    }
    void weakNounsAndPunctuationBreakRuns()
    {
        const auto k = KeywordExtractor::extract({ N("共有"), N("場合", true), N("フォルダ"), P(), N("権限"), N("管理") }, {});
        QCOMPARE(setOf(k), QSet<QString>({ "共有", "フォルダ", "権限管理" }));
    }
    void veryLongNounRunsAreTreatedAsLists()
    {
        const auto k = KeywordExtractor::extract({ N("りんご"), N("ミカン"), N("バナナ"), N("ぶどう"), N("メロン") }, {});
        // 5 nouns > kMaxRunTokens: individual nouns, not one 5-part "compound"
        QVERIFY(!terms(k).join("").contains("りんごミカン"));
        QVERIFY(terms(k).contains("ミカン"));
    }
    void stopwordsAndJunkAreDropped()
    {
        const auto k = KeywordExtractor::extract(
            { N("the"), N("Docker"), O("is"), N("is"), N("こと"), N("社"), N("ばあい"), N("Podman"), N("a") }, {});
        QCOMPARE(setOf(k), QSet<QString>({ "Docker", "Podman" }));  // "the Docker" -> Docker; hiragana-only & 1-char dropped
    }
    void caseVariantsMerge()
    {
        const auto k = KeywordExtractor::extract(repeat({ N("Docker") }, 1) + repeat({ N("docker") }, 1) + repeat({ N("DOCKER") }, 1), {});
        QCOMPARE(k.size(), 1);
        QCOMPARE(k[0].count, 3);
        QCOMPARE(k[0].term, QString("Docker")); // first form seen
    }
    void frequencyRaisesScoreButSubLinearly()
    {
        QList<Token> t = repeat({ N("会議録") }, 1) + repeat({ N("設計書") }, 8);
        const auto k = KeywordExtractor::extract(t, {});
        QCOMPARE(k[0].term, QString("設計書"));
        QVERIFY(k[0].score < 8 * k[1].score); // log tf, not raw tf
        QVERIFY(k[0].score > k[1].score);
    }
    void rareInLibraryBeatsCommonAtEqualFrequency()
    {
        QList<Token> t = repeat({ N("会議") }, 3) + repeat({ N("Podman") }, 3);
        CorpusStats stats;
        stats.otherDocuments = 20;
        stats.docFreq = [](const QString& w) { return w == "会議" ? 20 : w == "Podman" ? 1 : 5; };
        const auto k = KeywordExtractor::extract(t, stats);
        QCOMPARE(k[0].term, QString("Podman"));
        QVERIFY(k[0].score > 2 * k[1].score);
        // without corpus information the two tie, and the order is deterministic (by key)
        const auto k2 = KeywordExtractor::extract(t, {});
        QCOMPARE(terms(k2), QStringList({ "Podman", "会議" }));   // "podman" < "会議"
    }
    void compoundsGetASmallBonus()
    {
        QList<Token> t = repeat({ N("共有"), N("フォルダ") }, 2) + repeat({ N("確認") }, 2);
        const auto k = KeywordExtractor::extract(t, {});
        QCOMPARE(k[0].term, QString("共有フォルダ"));
    }
    void topKAndRunsDoNotCrossSentenceBreaks()
    {
        QList<Token> t;
        for (int i = 0; i < 20; ++i) t += QList<Token>{ N(QString("用語%1").arg(i, 2, 10, QChar('0'))), KeywordExtractor::sentenceBreak() };
        QCOMPARE(KeywordExtractor::extract(t, {}).size(), KeywordExtractor::kDefaultTopK);
        QCOMPARE(KeywordExtractor::extract(t, {}, 3).size(), 3);
        QCOMPARE(KeywordExtractor::extract(t, {}, 0).size(), 0);
        QCOMPARE(setOf(KeywordExtractor::extract({ N("検索"), KeywordExtractor::sentenceBreak(), N("エンジン") }, {})),
                 QSet<QString>({ "検索", "エンジン" }));
        QVERIFY(KeywordExtractor::extract({}, {}).isEmpty());
    }

    // ---- end to end with MeCab: a realistic passage (written for this test, not a recording)
    void realisticJapanesePassageYieldsSensibleKeywords()
    {
        if (mecab->name() != "mecab") QSKIP("MeCab not available");
        const QStringList sentences = {
            "今日はDocker ComposeからPodmanへの移行について議論します。",
            "移行の目的は、rootlessで動かせるのでセキュリティ面のメリットが大きいことです。",
            "systemdとの連携はQuadletを使う前提で考えます。",
            "移行は一度に行わず、段階的に進める場合が多いです。",
            "まずはCI環境でPodmanに切り替えて、問題がないか確認します。",
            "共有フォルダの権限もrootlessだと注意が必要です。",
            "来月からPodmanの検証を始めて、移行の計画を決めます。",
        };
        QList<Token> toks;
        for (const QString& s : sentences) toks += mecab->tokenize(s) + QList<Token>{ KeywordExtractor::sentenceBreak() };
        const auto k = KeywordExtractor::extract(toks, {}, 8);
        const QStringList got = terms(k);
        QVERIFY2(got.mid(0, 3).contains("移行"), qPrintable(got.join(" / ")));
        QVERIFY2(got.contains("Podman"), qPrintable(got.join(" / ")));
        QVERIFY2(got.contains("rootless"), qPrintable(got.join(" / ")));
        for (const char* bad : { "場合", "来月", "こと", "ので", "まず" }) QVERIFY2(!got.contains(bad), bad);
        for (const auto& kw : k) QVERIFY2(kw.term.size() >= 2, qPrintable(kw.term));
    }
};

QTEST_MAIN(TstTokenizer)
#include "tst_tokenizer.moc"
