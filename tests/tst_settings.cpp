#include <QFile>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include "core/Database.h"
#include "core/Settings.h"

using namespace ake;

class TstSettings : public QObject {
    Q_OBJECT
private slots:
    void emptyTableMeansDefaults()
    {
        Database db;
        QVERIFY(db.open(":memory:"));
        Settings s(db);
        QVERIFY(!s.contains(Settings::kTheme));
        QCOMPARE(s.value("nope", "fallback"), QString("fallback"));
        QCOMPARE(s.value("nope"), QString());
        QCOMPARE(s.theme(), QString("system"));
        QCOMPARE(s.viewMode(), QString("cards"));
        QCOMPARE(s.sortOrder(), QString("newest"));
        QVERIFY(s.autoAnalyze());
        QCOMPARE(s.scanIntervalSeconds(), Settings::kDefaultScanIntervalS);
        const AnalysisConfig c = s.analysisConfig();
        QCOMPARE(c.language, QString("ja"));                            // not "auto": detection took a Japanese interview for Norwegian
        QCOMPARE(c.threads, 0);
        QCOMPARE(c.beamSize, 5);                                        // accuracy first by default
        QVERIFY(c.suppressNst);                                         // no "(音声)" instead of words
        QVERIFY(c.modelPath.isEmpty());                                 // default location is resolved by the pipeline
    }
    void setGetOverwriteRemoveRoundTrip()
    {
        Database db;
        QVERIFY(db.open(":memory:"));
        Settings s(db);
        QVERIFY(s.set("k", "日本語の値 with spaces"));
        QCOMPARE(s.value("k"), QString("日本語の値 with spaces"));
        QVERIFY(s.contains("k"));
        QVERIFY(s.set("k", "second"));                                   // overwrite, not a duplicate row
        QCOMPARE(s.value("k"), QString("second"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("SELECT count(*) FROM settings WHERE key='k'"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1);
        QVERIFY(s.set("empty", QString()));                              // a null QString must not violate NOT NULL
        QVERIFY(s.contains("empty"));
        QCOMPARE(s.value("empty", "x"), QString(""));
        QVERIFY(s.remove("k"));
        QVERIFY(!s.contains("k"));
        QVERIFY(!s.remove("k"));
    }
    void typedValuesFallBackOnGarbageAndOutOfRange()
    {
        Database db;
        QVERIFY(db.open(":memory:"));
        Settings s(db);
        s.set(Settings::kThreads, "abc");
        QCOMPARE(s.analysisConfig().threads, 0);
        s.set(Settings::kThreads, "-3");
        QCOMPARE(s.analysisConfig().threads, 0);
        s.set(Settings::kThreads, "9999");
        QCOMPARE(s.analysisConfig().threads, 0);
        s.setInt(Settings::kThreads, 6);
        QCOMPARE(s.analysisConfig().threads, 6);
        for (const char* yes : { "1", "true", "TRUE", " yes ", "on" }) { s.set(Settings::kAutoAnalyze, yes); QVERIFY2(s.autoAnalyze(), yes); }
        for (const char* no : { "0", "false", "No", "off" }) { s.set(Settings::kAutoAnalyze, no); QVERIFY2(!s.autoAnalyze(), no); }
        s.set(Settings::kAutoAnalyze, "maybe");
        QVERIFY2(s.autoAnalyze(), "unknown text: the default (on)");
        s.setBool(Settings::kAutoAnalyze, false);
        QVERIFY(!s.autoAnalyze());
        s.set(Settings::kScanInterval, "0");
        QCOMPARE(s.scanIntervalSeconds(), 0);                            // 0 is valid: scanning off
        s.set(Settings::kScanInterval, "-1");
        QCOMPARE(s.scanIntervalSeconds(), Settings::kDefaultScanIntervalS);
    }
    void beamWidthIsValidated()
    {
        Database db;
        QVERIFY(db.open(":memory:"));
        Settings s(db);
        s.setInt(Settings::kBeam, 0);   QCOMPARE(s.analysisConfig().beamSize, 0);    // 0 = fast greedy is a real choice
        s.setInt(Settings::kBeam, 3);   QCOMPARE(s.analysisConfig().beamSize, 3);
        s.setInt(Settings::kBeam, 99);  QCOMPARE(s.analysisConfig().beamSize, 5);
        s.set(Settings::kBeam, "x");    QCOMPARE(s.analysisConfig().beamSize, 5);
    }
    void choicesAreValidated()
    {
        Database db;
        QVERIFY(db.open(":memory:"));
        Settings s(db);
        s.set(Settings::kTheme, "dark"); QCOMPARE(s.theme(), QString("dark"));
        s.set(Settings::kTheme, "neon"); QCOMPARE(s.theme(), QString("system"));
        s.set(Settings::kViewMode, "rows"); QCOMPARE(s.viewMode(), QString("rows"));
        s.set(Settings::kViewMode, "grid"); QCOMPARE(s.viewMode(), QString("cards"));
        s.set(Settings::kSortOrder, "longest"); QCOMPARE(s.sortOrder(), QString("longest"));
        s.set(Settings::kSortOrder, ""); QCOMPARE(s.sortOrder(), QString("newest"));
    }
    void analysisConfigReflectsTheStoredValues()
    {
        Database db;
        QVERIFY(db.open(":memory:"));
        Settings s(db);
        s.set(Settings::kModelPath, "  /models/ggml-large.bin ");
        s.set(Settings::kLanguage, " JA ");
        s.setInt(Settings::kThreads, 4);
        const AnalysisConfig c = s.analysisConfig();
        QCOMPARE(c.modelPath, QString("/models/ggml-large.bin"));
        QCOMPARE(c.language, QString("ja"));
        QCOMPARE(c.threads, 4);
        s.set(Settings::kLanguage, "  ");
        QCOMPARE(s.analysisConfig().language, QString("ja"));
    }
    void modelsAreDescribedByTheirFileName()
    {
        QVERIFY(Settings::describeModel("ggml-small").contains("誤変換"));
        QVERIFY(Settings::describeModel("ggml-large-v3-turbo").contains("お勧め"));
        QVERIFY(Settings::describeModel("ggml-large-v3").contains("最も高精度"));
        QVERIFY(Settings::describeModel("ggml-tiny.en").contains("最小"));
        const QString q = Settings::describeModel("ggml-large-v3-turbo-q5_0");   // a quantised turbo: both parts
        QVERIFY(q.contains("お勧め") && q.contains("量子化"));
        QVERIFY(Settings::describeModel("my-own-finetune").isEmpty());          // an unknown model: nothing invented
        QVERIFY(Settings::describeModel(QString()).isEmpty());
    }
    void settingsSurviveReopeningTheDatabase()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("lib.db");
        {
            Database db;
            QVERIFY(db.open(path));
            Settings s(db);
            s.set(Settings::kTheme, "dark");
            s.set(Settings::kViewMode, "rows");
        }
        Database db;
        QVERIFY(db.open(path));
        Settings s(db);
        QCOMPARE(s.theme(), QString("dark"));
        QCOMPARE(s.viewMode(), QString("rows"));
    }
    void installedModelsAreListedSmallestFirstAndOthersIgnored()
    {
        QTemporaryDir dir;
        auto make = [&](const QString& name, int size) { QFile f(dir.filePath(name)); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(QByteArray(size, 'x')); };
        make("ggml-small.bin", 500); make("ggml-tiny.bin", 70); make("ggml-large-v3-turbo.bin", 1600);
        make("notes.txt", 10); make("whisper.bin", 10); make("ggml-broken.gguf", 10);
        const auto models = Settings::installedModels(dir.path());
        QCOMPARE(models.size(), 3);
        QCOMPARE(models[0].name, QString("ggml-tiny"));
        QCOMPARE(models[1].name, QString("ggml-small"));
        QCOMPARE(models[2].name, QString("ggml-large-v3-turbo"));
        QCOMPARE(models[0].sizeBytes, qint64(70));
        QVERIFY(models[2].path.endsWith("ggml-large-v3-turbo.bin"));
        QVERIFY(Settings::installedModels(dir.filePath("missing-dir")).isEmpty());
    }
};

QTEST_MAIN(TstSettings)
#include "tst_settings.moc"
