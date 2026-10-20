#include <QSqlQuery>
#include <QtTest>

#include "core/Database.h"
#include "core/ITokenizer.h"
#include "core/NgramTokenizer.h"
#include "core/PhoneticMatch.h"
#include "core/TextIndexer.h"
#include "core/TranscriptRepository.h"

using namespace ake;
namespace PM = ake::PhoneticMatch;

class TstPhonetic : public QObject {
    Q_OBJECT
    std::unique_ptr<ITokenizer> tok;

private slots:
    void initTestCase() { tok = createTokenizer(); }

    // ---- the matcher
    void distanceIsTheBestEditCostOfAnySubstring()
    {
        QCOMPARE(PM::distance("かいとう", "はいとうのだんかい", 2), 1);        // か -> は
        QCOMPARE(PM::distance("かいとう", "それはかいとうです", 2), 0);        // exact, inside
        QCOMPARE(PM::distance("かいとう", "かとう", 2), 1);                    // one kana missing
        QCOMPARE(PM::distance("かいとう", "かいいとう", 2), 1);                // one too many
        QCOMPARE(PM::distance("かいとう", "ちばへ", 2), 3);                    // beyond the limit: maxDistance + 1
        QCOMPARE(PM::distance("かいとう", "", 2), 3);
        QCOMPARE(PM::distance("", "なんでも", 2), 0);
        QCOMPARE(PM::distance("けんとう", "せっていをけんとう", 1), 0);        // the tail of a longer text
    }
    void allowedDistanceGrowsWithTheLengthAndShortWordsAreNotMatched()
    {
        QCOMPARE(PM::allowedDistance(2), -1);
        QCOMPARE(PM::allowedDistance(3), -1);
        QCOMPARE(PM::allowedDistance(4), 1);
        QCOMPARE(PM::allowedDistance(6), 1);
        QCOMPARE(PM::allowedDistance(7), 2);
        QCOMPARE(PM::allowedDistance(12), 2);
    }
    void onlyKanaReadingsAreCompared()
    {
        QVERIFY(PM::isKanaOnly("かいとう"));
        QVERIFY(PM::isKanaOnly("らーめん"));                                   // the long vowel mark belongs to kana
        QVERIFY(!PM::isKanaOnly("podman"));
        QVERIFY(!PM::isKanaOnly("3けん"));
        QVERIFY(!PM::isKanaOnly("かいとうa"));
        QVERIFY(!PM::isKanaOnly(""));
    }

    // ---- readings from the tokenizer
    void katakanaBecomesHiraganaAndNothingElseChanges()
    {
        QCOMPARE(ITokenizer::toHiragana("カイトウ・ラーメン"), QString("かいとう・らーめん"));
        QCOMPARE(ITokenizer::toHiragana("Podman 検討"), QString("Podman 検討"));
    }
    void aDictionaryTokenizerKnowsReadingsTheFallbackDoesNot()
    {
        QCOMPARE(tok->hasReadings(), tok->name() == "mecab");
        QVERIFY(!NgramTokenizer().hasReadings());
        // the fallback: only the kana are unified, the rest stays as written
        QCOMPARE(NgramTokenizer().phonetic("カイトウ Podman。"), QString("かいとうpodman"));
    }
    void homophonesShareAPhoneticForm()
    {
        if (!tok->hasReadings()) QSKIP("needs a tokenizer with readings (MeCab)");
        QCOMPARE(tok->phonetic("検討"), tok->phonetic("見当"));
        QCOMPARE(tok->phonetic("検討"), QString("けんとう"));
        QCOMPARE(tok->phonetic("接続検討、回答。"), QString("せつぞくけんとうかいとう"));            // punctuation is dropped
        QCOMPARE(tok->phonetic("Podmanは3件です"), QString("podmanは3けんです"));                       // unread words stay, lower-cased
        QVERIFY(tok->phonetic("").isEmpty());
        QVERIFY(tok->phonetic("。、").isEmpty());
    }

    // ---- stored by the indexer
    void theIndexerStoresThePhoneticFormOfEverySegmentFromTheEditedText()
    {
        Database db;
        QVERIFY(db.open(":memory:"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("INSERT INTO folders(id,path) VALUES(1,'/f')"));
        QVERIFY(q.exec("INSERT INTO recordings(id,folder_id,rel_path,abs_path,status,created_at,updated_at) VALUES(1,1,'a.mp3','/f/a.mp3','done',0,0)"));
        TranscriptResult r; r.language = "ja"; r.model = "t";
        r.segments = { { 0, 3000, "設定を検討します" }, { 3000, 6000, "回答はまだです" } };
        QVERIFY(TranscriptRepository(db).save(1, r));
        QVERIFY(TextIndexer(db, *tok).index(1));
        auto phon = [&](int idx) { QSqlQuery x(db.handle()); x.exec(QString("SELECT phonetic FROM segments WHERE idx=%1").arg(idx)); x.next(); return x.value(0).toString(); };
        if (tok->hasReadings()) {
            QVERIFY2(phon(0).contains("けんとう"), qPrintable(phon(0)));
            QVERIFY2(phon(1).startsWith("かいとう"), qPrintable(phon(1)));
            QVERIFY(q.exec("UPDATE segments SET edited_text='配当の段階' WHERE idx=1"));
            QVERIFY(TextIndexer(db, *tok).index(1));
            QVERIFY2(phon(1).startsWith("はいとう"), qPrintable(phon(1)));                                // the user's wording
        } else {
            QVERIFY(phon(0).isEmpty());                                                                   // no readings: nothing stored
        }
    }
};

QTEST_MAIN(TstPhonetic)
#include "tst_phonetic.moc"
