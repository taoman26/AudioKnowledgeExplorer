#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QMenu>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>

#include "core/Database.h"
#include "core/ITokenizer.h"
#include "core/JobScheduler.h"
#include "core/Settings.h"
#include "ui/CardListView.h"
#include "ui/MainWindow.h"
#include "ui/RecordingListModel.h"
#include "ui/SettingsDialog.h"
#include "Fakes.h"

using namespace ake;

class TstSettingsUi : public QObject {
    Q_OBJECT
    Database db;
    fakes::FakeDecoder dec;
    std::unique_ptr<ITokenizer> tok;
    std::unique_ptr<JobScheduler> sched;
    std::unique_ptr<MainWindow> w;

    void start()
    {
        sched = std::make_unique<JobScheduler>(db, dec, fakes::factory());
        w = std::make_unique<MainWindow>();
        w->setTokenizer(tok.get());
        w->setDatabase(&db);
        w->setScheduler(sched.get());
        w->show();
        QVERIFY(QTest::qWaitForWindowActive(w.get()));
    }
    static QColor windowColor(QWidget* wd) { return wd->palette().color(QPalette::Window); }

private slots:
    void init() { tok = createTokenizer(); QVERIFY(db.open(":memory:")); }
    void cleanup() { w.reset(); sched.reset(); }

    void theDialogShowsTheStoredValuesAndSavesOnlyOnOk()
    {
        Settings st(db);
        st.set(Settings::kLanguage, "ja");
        st.setInt(Settings::kThreads, 6);
        st.setBool(Settings::kAutoAnalyze, false);
        st.setInt(Settings::kScanInterval, 120);
        st.set(Settings::kTheme, "dark");
        SettingsDialog d(st);
        QCOMPARE(d.languageCombo()->currentData().toString(), QString("ja"));
        QCOMPARE(d.threadsCombo()->currentData().toInt(), 6);
        QVERIFY(!d.autoAnalyzeBox()->isChecked());
        QCOMPARE(d.scanIntervalCombo()->currentData().toInt(), 120);                 // not a preset: kept selectable
        QVERIFY(d.scanIntervalCombo()->currentText().contains("120"));
        QCOMPARE(d.themeCombo()->currentData().toString(), QString("dark"));
        d.languageCombo()->setCurrentIndex(d.languageCombo()->findData("en"));
        d.threadsCombo()->setCurrentIndex(d.threadsCombo()->findData(0));
        d.themeCombo()->setCurrentIndex(d.themeCombo()->findData("light"));
        d.reject();                                                              // cancel: nothing is written
        QCOMPARE(st.value(Settings::kLanguage), QString("ja"));
        QCOMPARE(st.theme(), QString("dark"));
        d.accept();
        QCOMPARE(st.analysisConfig().language, QString("en"));
        QCOMPARE(st.analysisConfig().threads, 0);                                 // "自動" is stored as 0
        QCOMPARE(st.theme(), QString("light"));
        QVERIFY(!st.autoAnalyze());
        QCOMPARE(st.scanIntervalSeconds(), 120);
        QCOMPARE(d.threadsCombo()->itemText(d.threadsCombo()->findData(0)), QString("自動"));
    }
    void watchingTheFoldersIsAChoiceOfItsOwn()
    {
        Settings st(db);
        QVERIFY(st.watchFolders());                                              // on by default
        {
            SettingsDialog d(st);
            QVERIFY(d.watchBox()->isChecked());
            QVERIFY(d.scanIntervalCombo()->isEnabled());
            d.watchBox()->setChecked(false);
            QVERIFY(!d.scanIntervalCombo()->isEnabled());                         // the interval means nothing while not watching
            d.scanIntervalCombo()->setCurrentIndex(d.scanIntervalCombo()->findData(300));
            d.accept();
        }
        QVERIFY(!st.watchFolders());
        QCOMPARE(st.scanIntervalSeconds(), 300);                                 // the interval is kept for when it is turned on again
        {
            SettingsDialog d(st);
            QVERIFY(!d.watchBox()->isChecked());
            d.watchBox()->setChecked(true);
            d.accept();
        }
        QVERIFY(st.watchFolders());
        st.set(Settings::kScanInterval, "0");                                    // what older versions stored for "never"
        QVERIFY(!st.watchFolders());
        SettingsDialog old(st);
        QVERIFY(!old.watchBox()->isChecked());
        QCOMPARE(old.scanIntervalCombo()->currentData().toInt(), Settings::kDefaultScanIntervalS);
    }
    void modelChoiceListsInstalledModelsCustomFilesAndWarnsWhenMissing()
    {
        Settings st(db);
        SettingsDialog d(st);
        QVERIFY2(d.modelCombo()->itemText(0).contains("標準"), qPrintable(d.modelCombo()->itemText(0)));
        QCOMPARE(d.modelCombo()->currentData().toString(), QString());           // default location
        QTemporaryDir dir;
        QFile f(dir.filePath("ggml-mine.bin"));
        QVERIFY(f.open(QIODevice::WriteOnly)); f.write("x"); f.close();
        d.setBrowser([&] { return f.fileName(); });
        d.addCustomModel(f.fileName());
        QCOMPARE(d.modelCombo()->currentData().toString(), f.fileName());
        QVERIFY(!d.modelNote()->property("error").toBool());                     // file exists
        QCOMPARE(d.modelNote()->text(), f.fileName());
        const int n = d.modelCombo()->count();
        d.addCustomModel(f.fileName());                                           // choosing it again adds no duplicate
        QCOMPARE(d.modelCombo()->count(), n);
        d.addCustomModel(dir.filePath("ggml-gone.bin"));                          // a file that is not there
        QVERIFY(d.modelNote()->property("error").toBool());
        QVERIFY(d.modelNote()->text().contains("見つかりません"));
        d.accept();
        QCOMPARE(st.analysisConfig().modelPath, dir.filePath("ggml-gone.bin"));
        // reopening shows the stored custom model as the selection
        SettingsDialog again(st);
        QCOMPARE(again.modelCombo()->currentData().toString(), dir.filePath("ggml-gone.bin"));
    }
    void anUnknownLanguageCodeStaysSelectable()
    {
        Settings st(db);
        st.set(Settings::kLanguage, "it");
        SettingsDialog d(st);
        QCOMPARE(d.languageCombo()->currentData().toString(), QString("it"));
        d.accept();
        QCOMPARE(st.analysisConfig().language, QString("it"));
    }

    void okInTheWindowDialogAppliesEverythingAtOnce()
    {
        start();
        Settings st(db);
        QVERIFY(w->settingsAction());
        QCOMPARE(w->settingsAction()->shortcut(), QKeySequence("Ctrl+,"));
        QSignalSpy changed(w.get(), &MainWindow::settingsChanged);
        const QColor before = windowColor(w.get());
        SettingsDialog* d = w->createSettingsDialog();
        QVERIFY(d);
        d->languageCombo()->setCurrentIndex(d->languageCombo()->findData("ja"));
        d->threadsCombo()->setCurrentIndex(d->threadsCombo()->findData(4));
        QCOMPARE(d->qualityCombo()->currentData().toInt(), 5);                    // the default
        d->qualityCombo()->setCurrentIndex(d->qualityCombo()->findData(0));
        d->themeCombo()->setCurrentIndex(d->themeCombo()->findData("dark"));
        d->show();
        QTest::mouseClick(d->okButton(), Qt::LeftButton);
        QCOMPARE(sched->config().language, QString("ja"));                        // the scheduler uses it for the next job
        QCOMPARE(sched->config().threads, 4);
        QCOMPARE(sched->config().beamSize, 0);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(windowColor(w.get()), Theme::dark().bg);
        // and back to light
        d = w->createSettingsDialog();
        d->themeCombo()->setCurrentIndex(d->themeCombo()->findData("light"));
        d->show();
        QTest::mouseClick(d->okButton(), Qt::LeftButton);
        QCOMPARE(windowColor(w.get()), Theme::light().bg);
        QVERIFY(before == Theme::light().bg || before == Theme::dark().bg);
    }
    void followingTheSystemUsesThePaletteTheWindowStartedWith()
    {
        start();
        const QColor system = windowColor(w.get());                               // the system theme (offscreen default)
        w->applyThemeName("dark");
        QCOMPARE(windowColor(w.get()), Theme::dark().bg);
        w->applyThemeName("system");
        QCOMPARE(windowColor(w.get()), system);                                   // not stuck on the forced palette
        w->applyThemeName("nonsense");                                            // garbage means "system" as well
        QCOMPARE(windowColor(w.get()), system);
    }
    void viewModeAndSortOrderComeBackAfterARestart()
    {
        start();
        QCOMPARE(w->cardView()->mode(), CardListView::Mode::Cards);               // defaults
        QCOMPARE(w->recordingModel()->sortOrder(), RecordingListModel::SortOrder::Newest);
        QTest::mouseClick(w->rowsViewButton(), Qt::LeftButton);                   // the user clicks the list button
        QCOMPARE(Settings(db).viewMode(), QString("rows"));
        w->sortButton()->menu()->actions().at(1)->trigger();                       // 古い順
        QCOMPARE(Settings(db).sortOrder(), QString("oldest"));
        w.reset();                                                                // "quit"
        sched.reset();
        start();                                                                  // "start again" on the same database
        QCOMPARE(w->cardView()->mode(), CardListView::Mode::Rows);
        QCOMPARE(w->recordingModel()->sortOrder(), RecordingListModel::SortOrder::Oldest);
        QVERIFY(w->rowsViewButton()->isChecked());
        QVERIFY(w->sortButton()->text().contains("古い順"));
        QTest::mouseClick(w->cardsViewButton(), Qt::LeftButton);
        QCOMPARE(Settings(db).viewMode(), QString("cards"));
        w->sortButton()->menu()->actions().at(2)->trigger();
        QCOMPARE(Settings(db).sortOrder(), QString("longest"));
    }
    void settingCodeDrivenChangesDoesNotOverwriteTheUsersChoice()
    {
        start();
        w->setViewMode(CardListView::Mode::Rows);                                 // e.g. the --list debug option
        QCOMPARE(Settings(db).viewMode(), QString("cards"));
        QVERIFY(!Settings(db).contains(Settings::kViewMode));
    }
    void aBrokenStoredValueFallsBackToTheDefaults()
    {
        Settings st(db);
        st.set(Settings::kViewMode, "hologram");
        st.set(Settings::kSortOrder, "sideways");
        st.set(Settings::kTheme, "neon");
        start();
        QCOMPARE(w->cardView()->mode(), CardListView::Mode::Cards);
        QCOMPARE(w->recordingModel()->sortOrder(), RecordingListModel::SortOrder::Newest);
    }
};

QTEST_MAIN(TstSettingsUi)
#include "tst_settings_ui.moc"
