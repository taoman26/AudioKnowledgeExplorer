#include <QAction>
#include <QCheckBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPushButton>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest>

#include "core/Database.h"
#include "core/ITokenizer.h"
#include "core/JobScheduler.h"
#include "core/Library.h"
#include "core/ScanService.h"
#include "core/Settings.h"
#include "ui/CardListView.h"
#include "ui/FolderDialog.h"
#include "ui/MainWindow.h"
#include "ui/RecordingListModel.h"
#include "ui/SidebarWidget.h"
#include "Fakes.h"

using namespace ake;

namespace {
void write(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(path));
    f.write(data);
}
QByteArray bytes(const char* tag) { return QByteArray(tag) + QByteArray(3000, 'x'); }
} // namespace

class TstFolders : public QObject {
    Q_OBJECT
    Database db;
    QTemporaryDir dir;
    fakes::FakeDecoder dec;
    std::unique_ptr<ScanService> scan;
    std::unique_ptr<FolderDialog> dlg;
    QString chosen;
    bool confirmAnswer = true;
    QString confirmText;

    QString root(const QString& n) const { return dir.path() + "/" + n; }
    int count(const QString& sql)
    {
        QSqlQuery q(db.handle());
        return (q.exec(sql) && q.next()) ? q.value(0).toInt() : -1;
    }
    void openDialog()
    {
        dlg = std::make_unique<FolderDialog>(db, *scan);
        dlg->setChooser([this] { return chosen; });
        dlg->setConfirm([this](const QString&, const QString& text) { confirmText = text; return confirmAnswer; });
        dlg->show();
        QVERIFY(QTest::qWaitForWindowExposed(dlg.get()));
    }
    qint64 firstFolder() { return Library(db).folders().first().id; }

private slots:
    void init()
    {
        QDir(dir.path()).removeRecursively();
        QDir().mkpath(dir.path());
        QVERIFY(db.open(":memory:"));
        scan = std::make_unique<ScanService>(db, &dec);
        chosen.clear(); confirmAnswer = true; confirmText.clear();
    }
    void cleanup() { dlg.reset(); scan.reset(); }

    void startsEmptyWithOnlyAddEnabled()
    {
        openDialog();
        QCOMPARE(dlg->tree()->topLevelItemCount(), 0);
        QVERIFY(dlg->addButton()->isEnabled());
        QVERIFY(!dlg->removeButton()->isEnabled());
        QVERIFY(!dlg->rescanButton()->isEnabled());
        QVERIFY(!dlg->excludeEdit()->isEnabled());
        QVERIFY(!dlg->recursiveBox()->isEnabled());
    }
    void addingAFolderRegistersScansAndShowsTheProgress()
    {
        write(root("rec") + "/a.mp3", bytes("a")); write(root("rec") + "/sub/b.wav", bytes("b"));
        openDialog();
        QSignalSpy changed(dlg.get(), &FolderDialog::libraryChanged);
        chosen = root("rec");
        QTest::mouseClick(dlg->addButton(), Qt::LeftButton);
        QCOMPARE(dlg->tree()->topLevelItemCount(), 1);
        const int row = 0;
        QCOMPARE(dlg->tree()->topLevelItem(row)->text(FolderDialog::kPathCol), QFileInfo(root("rec")).canonicalFilePath());   // /tmp may be a symlink (Haiku)
        QVERIFY2(dlg->statusOf(row).contains("走査中"), qPrintable(dlg->statusOf(row)));
        QVERIFY(dlg->message().contains("追加しました"));
        QVERIFY(changed.count() >= 1);
        QTRY_VERIFY_WITH_TIMEOUT(dlg->statusOf(0).contains("同期済"), 5000);
        QCOMPARE(dlg->tree()->topLevelItem(0)->text(FolderDialog::kCountCol), QString("2本"));
        QCOMPARE(dlg->tree()->topLevelItem(0)->text(FolderDialog::kRecursiveCol), QString("含む"));
        QVERIFY2(dlg->message().contains("追加 2"), qPrintable(dlg->message()));
        QCOMPARE(dlg->tree()->currentItem(), dlg->tree()->topLevelItem(0));      // the new folder is selected
        QVERIFY(dlg->removeButton()->isEnabled() && dlg->rescanButton()->isEnabled());
        QVERIFY(dlg->recursiveBox()->isChecked());
    }
    void rejectedFoldersExplainWhyAndCancelDoesNothing()
    {
        write(root("rec") + "/a.mp3", bytes("a"));
        Library(db).addFolder(root("rec"));
        openDialog();
        auto tryAdd = [&](const QString& path) { chosen = path; QTest::mouseClick(dlg->addButton(), Qt::LeftButton); };
        tryAdd(QString());                                                    // dialog cancelled
        QCOMPARE(dlg->tree()->topLevelItemCount(), 1);
        QVERIFY(dlg->message().isEmpty());
        tryAdd(root("rec"));                                                  // the same folder
        QVERIFY2(dlg->message().contains("追加できません"), qPrintable(dlg->message()));
        QVERIFY(dlg->message().contains("already"));
        QCOMPARE(dlg->tree()->topLevelItemCount(), 1);
        QVERIFY(QDir().mkpath(root("rec") + "/inner"));
        tryAdd(root("rec") + "/inner");                                       // inside a registered folder
        QVERIFY(dlg->message().contains("overlaps"));
        tryAdd(root("does-not-exist"));
        QVERIFY(dlg->message().contains("not a directory"));
        QCOMPARE(dlg->tree()->topLevelItemCount(), 1);
        QCOMPARE(dlg->findChild<QLabel*>("folderMessage")->property("error").toBool(), true);   // shown as an error
    }
    void removingNeedsConfirmationAndKeepsTheAudioFiles()
    {
        write(root("rec") + "/a.mp3", bytes("a")); write(root("rec") + "/b.mp3", bytes("b"));
        const qint64 id = Library(db).addFolder(root("rec"));
        scan->scanFolder(id);
        QVERIFY(scan->waitUntilIdle(5000));
        openDialog();
        QSignalSpy changed(dlg.get(), &FolderDialog::libraryChanged);
        confirmAnswer = false;
        QTest::mouseClick(dlg->removeButton(), Qt::LeftButton);
        QCOMPARE(dlg->tree()->topLevelItemCount(), 1);                        // "cancel": nothing happens
        QCOMPARE(count("SELECT count(*) FROM recordings"), 2);
        QVERIFY2(confirmText.contains("2 本") && confirmText.contains("音声ファイルそのものは削除されません"), qPrintable(confirmText));
        confirmAnswer = true;
        QTest::mouseClick(dlg->removeButton(), Qt::LeftButton);
        QCOMPARE(dlg->tree()->topLevelItemCount(), 0);
        QCOMPARE(count("SELECT count(*) FROM folders"), 0);
        QCOMPARE(count("SELECT count(*) FROM recordings"), 0);                // the library entries are gone ...
        QVERIFY(QFile::exists(root("rec") + "/a.mp3"));                       // ... the audio files are not
        QVERIFY(QFile::exists(root("rec") + "/b.mp3"));
        QVERIFY(changed.count() >= 1);
        QVERIFY(!dlg->removeButton()->isEnabled());
    }
    void rescanPicksUpNewFilesAndRecursionAndExcludeAreApplied()
    {
        write(root("rec") + "/a.mp3", bytes("a")); write(root("rec") + "/sub/b.mp3", bytes("b"));
        const qint64 id = Library(db).addFolder(root("rec"));
        scan->scanFolder(id);
        QVERIFY(scan->waitUntilIdle(5000));
        openDialog();
        QCOMPARE(dlg->tree()->topLevelItem(0)->text(FolderDialog::kCountCol), QString("2本"));
        // rescan finds a new file
        write(root("rec") + "/new.ogg", bytes("n"));
        QTest::mouseClick(dlg->rescanButton(), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(dlg->tree()->topLevelItem(0)->text(FolderDialog::kCountCol), QString("3本"), 5000);
        // turning recursion off drops the sub folder's recording
        QTest::mouseClick(dlg->recursiveBox(), Qt::LeftButton);
        QVERIFY(!dlg->recursiveBox()->isChecked());
        QTRY_COMPARE_WITH_TIMEOUT(dlg->tree()->topLevelItem(0)->text(FolderDialog::kCountCol), QString("2本"), 5000);
        QCOMPARE(dlg->tree()->topLevelItem(0)->text(FolderDialog::kRecursiveCol), QString("—"));
        // an exclude pattern, applied when the field is left
        QSignalSpy started(scan.get(), &ScanService::started);
        dlg->excludeEdit()->setText("*.ogg");
        dlg->applyExclude();
        QCOMPARE(Library(db).folders().first().excludeGlob, QString("*.ogg"));
        QTRY_COMPARE_WITH_TIMEOUT(dlg->tree()->topLevelItem(0)->text(FolderDialog::kCountCol), QString("1本"), 5000);
        QCOMPARE(started.count(), 1);
        dlg->applyExclude();                                                  // unchanged text: no needless rescan
        QTest::qWait(50);
        QCOMPARE(started.count(), 1);
    }
    void selectingAFolderShowsItsOwnOptions()
    {
        write(root("a") + "/1.mp3", bytes("a")); write(root("b") + "/1.mp3", bytes("b"));
        Library lib(db);
        const qint64 a = lib.addFolder(root("a"), true, "*.tmp"), b = lib.addFolder(root("b"), false, "");
        openDialog();
        dlg->selectFolder(a);
        QCOMPARE(dlg->excludeEdit()->text(), QString("*.tmp"));
        QVERIFY(dlg->recursiveBox()->isChecked());
        dlg->selectFolder(b);
        QCOMPARE(dlg->excludeEdit()->text(), QString(""));
        QVERIFY(!dlg->recursiveBox()->isChecked());
        QCOMPARE(dlg->tree()->topLevelItem(dlg->rowForFolder(b))->text(FolderDialog::kRecursiveCol), QString("—"));
    }
    void anUnmountedFolderShowsAWarningAndKeepsItsRecordings()
    {
        write(root("rec") + "/a.mp3", bytes("a"));
        const qint64 id = Library(db).addFolder(root("rec"));
        scan->scanFolder(id);
        QVERIFY(scan->waitUntilIdle(5000));
        openDialog();
        QVERIFY(QDir(root("rec")).removeRecursively());                       // "unplug the drive"
        QTest::mouseClick(dlg->rescanButton(), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(dlg->statusOf(0).startsWith("⚠"), 5000);
        QVERIFY2(dlg->statusOf(0).contains("アクセスできません"), qPrintable(dlg->statusOf(0)));
        QCOMPARE(dlg->findChild<QLabel*>("folderMessage")->property("error").toBool(), true);
        QCOMPARE(dlg->tree()->topLevelItem(0)->text(FolderDialog::kCountCol), QString("1本"));   // nothing was deleted
    }
};

class TstFoldersWindow : public QObject {
    Q_OBJECT
    Database db;
    QTemporaryDir dir;
    fakes::FakeDecoder dec;
    std::unique_ptr<ITokenizer> tok;
    std::unique_ptr<ScanService> scan;
    std::unique_ptr<JobScheduler> sched;
    std::unique_ptr<MainWindow> w;
    QString chosen;

    QString root(const QString& n) const { return dir.path() + "/" + n; }
    int rows() { return w->recordingModel()->rowCount(); }
    void start()
    {
        scan = std::make_unique<ScanService>(db, &dec);
        sched = std::make_unique<JobScheduler>(db, dec, fakes::factory());
        sched->setTokenizer(tok.get());
        w = std::make_unique<MainWindow>();
        w->setTokenizer(tok.get());
        w->setDatabase(&db);
        w->setScheduler(sched.get());
        w->setScanService(scan.get());
        w->setFolderChooser([this] { return chosen; });
        w->show();
        QVERIFY(QTest::qWaitForWindowActive(w.get()));
    }
    static QMimeData* urls(const QStringList& paths)
    {
        auto* m = new QMimeData;
        QList<QUrl> l;
        for (const QString& p : paths) l << QUrl::fromLocalFile(p);
        m->setUrls(l);
        return m;
    }
    void drop(const QStringList& paths)
    {
        std::unique_ptr<QMimeData> m(urls(paths));
        QDropEvent e(QPointF(100, 100), Qt::CopyAction, m.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(w.get(), &e);
    }

private slots:
    void init()
    {
        QDir(dir.path()).removeRecursively();
        QDir().mkpath(dir.path());
        tok = createTokenizer();
        QVERIFY(db.open(":memory:"));
        chosen.clear();
        fakes::g_stepMs = 5;
    }
    void cleanup() { w.reset(); sched.reset(); scan.reset(); }

    void theToolbarButtonRegistersAFolderAndTheCardsAppear()
    {
        write(root("rec") + "/meeting.m4a", bytes("m")); write(root("rec") + "/memo.wav", bytes("w"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("UPDATE settings SET value='0' WHERE key='analysis.auto'"));
        Settings(db).setBool(Settings::kAutoAnalyze, false);                  // keep the recordings "new" for this test
        start();
        QCOMPARE(rows(), 0);
        QVERIFY(w->addFolderAction()->text().contains("フォルダ追加"));
        chosen = root("rec");
        w->addFolderAction()->trigger();
        QVERIFY(w->statusBar()->currentMessage().contains("フォルダを追加しました"));
        QVERIFY(w->sidebarWidget()->findItem(SidebarWidget::Folder, Library(db).folders().first().id));   // listed at once
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, 5000);                           // the scan finished in the background
        QTRY_VERIFY(!w->scanLabel()->isVisible());
        QCOMPARE(w->recordingModel()->index(0, 0).data(RecordingListModel::StateRole).value<CardState>(), CardState::Pending);
    }
    void theScanIndicatorShowsWhileScanning()
    {
        for (int i = 0; i < 4; ++i) write(root("rec") + QString("/f%1.mp3").arg(i), bytes(QByteArray::number(i)));
        Settings(db).setBool(Settings::kAutoAnalyze, false);
        start();
        struct Slow : IDecoder { bool probe(const QString&, AudioInfo*, QString*) override { QThread::msleep(120); return true; }
                                 bool decode(const QString&, std::vector<float>*, AudioInfo*, QString*, const Progress&) override { return false; } } slow;
        scan = std::make_unique<ScanService>(db, &slow);                        // a slower scanner for this test
        w->setScanService(scan.get());
        chosen = root("rec");
        w->addFolderAction()->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(w->scanLabel()->isVisible(), 1000);
        QVERIFY(w->scanLabel()->text().contains("走査中"));
        QTRY_VERIFY_WITH_TIMEOUT(!w->scanLabel()->isVisible(), 8000);
        QCOMPARE(rows(), 4);
    }
    void newRecordingsAreAnalysedAutomaticallyUnlessTurnedOff()
    {
        for (bool autoOn : { true, false }) {
            cleanup();
            QVERIFY(db.open(":memory:"));
            QDir(root("rec")).removeRecursively();
            write(root("rec") + "/a.mp3", bytes("a")); write(root("rec") + "/b.mp3", bytes("b"));
            Settings(db).setBool(Settings::kAutoAnalyze, autoOn);
            start();
            chosen = root("rec");
            w->addFolderAction()->trigger();
            QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, 5000);
            if (autoOn) {
                QTRY_VERIFY_WITH_TIMEOUT(!sched->isBusy() && w->recordingModel()->counts().done == 2, 15000);
            } else {
                QTest::qWait(300);
                QVERIFY(!sched->isBusy());
                QCOMPARE(w->recordingModel()->counts().done, 0);               // still waiting for the user
                QCOMPARE(w->recordingModel()->counts().pending, 2);
            }
        }
    }
    void droppingFoldersRegistersThemAndIgnoresFiles()
    {
        write(root("one") + "/a.mp3", bytes("a")); write(root("two") + "/b.mp3", bytes("b")); write(root("loose.mp3"), bytes("l"));
        Settings(db).setBool(Settings::kAutoAnalyze, false);
        start();
        // drag enter: accepted for folders, ignored for plain files
        {
            std::unique_ptr<QMimeData> m(urls({ root("one") }));
            QDragEnterEvent in(QPoint(50, 50), Qt::CopyAction, m.get(), Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(w.get(), &in);
            QVERIFY(in.isAccepted());
            std::unique_ptr<QMimeData> f(urls({ root("loose.mp3") }));
            QDragEnterEvent fileIn(QPoint(50, 50), Qt::CopyAction, f.get(), Qt::LeftButton, Qt::NoModifier);
            fileIn.setAccepted(false);
            QApplication::sendEvent(w.get(), &fileIn);
            QVERIFY(!fileIn.isAccepted());
        }
        drop({ root("one"), root("loose.mp3"), root("two") });
        QCOMPARE(Library(db).folders().size(), 2);                              // the two folders, not the file
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, 5000);
        drop({ root("loose.mp3") });
        QCOMPARE(Library(db).folders().size(), 2);
    }
    void rejectedFoldersAreExplainedInTheStatusBar()
    {
        write(root("rec") + "/a.mp3", bytes("a"));
        Settings(db).setBool(Settings::kAutoAnalyze, false);
        start();
        QVERIFY(w->addFolder(root("rec")));
        QVERIFY(!w->addFolder(root("rec")));                                   // again
        QVERIFY(w->statusBar()->currentMessage().contains("追加できません"));
        QVERIFY(!w->addFolder(root("missing")));
        QVERIFY(w->statusBar()->currentMessage().contains("not a directory"));
        chosen = QString();
        w->addFolderAction()->trigger();                                        // chooser cancelled
        QCOMPARE(Library(db).folders().size(), 1);
    }
    void theSelectionSurvivesTheRefreshAfterAScan()
    {
        write(root("rec") + "/a.mp3", bytes("a"));
        Settings(db).setBool(Settings::kAutoAnalyze, false);
        start();
        QVERIFY(w->addFolder(root("rec")));
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, 5000);
        w->cardView()->setCurrentIndex(w->recordingModel()->index(0, 0));
        const qint64 selected = w->detailPaneWidget() ? w->cardView()->currentIndex().data(RecordingListModel::IdRole).toLongLong() : 0;
        QVERIFY(selected > 0);
        write(root("rec") + "/b.mp3", bytes("b"));                              // a second file shows up
        scan->scanAll();
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 2, 5000);
        QCOMPARE(w->cardView()->currentIndex().data(RecordingListModel::IdRole).toLongLong(), selected);
    }
    void anUnmountedFolderIsReportedAndItsCardsRemain()
    {
        write(root("rec") + "/a.mp3", bytes("a"));
        Settings(db).setBool(Settings::kAutoAnalyze, false);
        start();
        QVERIFY(w->addFolder(root("rec")));
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, 5000);
        QVERIFY(QDir(root("rec")).removeRecursively());
        scan->scanAll();
        QTRY_VERIFY_WITH_TIMEOUT(w->statusBar()->currentMessage().contains("走査できません"), 5000);
        QCOMPARE(rows(), 1);
    }
    void theFolderDialogOfTheWindowRefreshesTheLibraryWhenItChangesThings()
    {
        write(root("rec") + "/a.mp3", bytes("a"));
        Settings(db).setBool(Settings::kAutoAnalyze, false);
        start();
        FolderDialog* d = w->createFolderDialog();
        QVERIFY(d);
        d->setChooser([&] { return root("rec"); });
        d->show();
        QTest::mouseClick(d->addButton(), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(rows(), 1, 5000);                             // the cards behind the dialog updated
        d->setConfirm([](const QString&, const QString&) { return true; });
        QTest::mouseClick(d->removeButton(), Qt::LeftButton);
        QCOMPARE(rows(), 0);
        d->close();
    }
    void emptyStateFollowsTheLibrary()
    {
        write(root("rec") + "/a.mp3", bytes("a"));
        Settings(db).setBool(Settings::kAutoAnalyze, false);
        start();
        using K = MainWindow::EmptyKind;
        QCOMPARE(w->emptyKind(), K::NoFolders);
        QVERIFY(w->emptyState()->isVisible());
        QVERIFY(!w->cardView()->isVisible());
        QVERIFY(w->emptyAddButton()->isVisible());
        QVERIFY(w->emptyTitle()->text().contains("登録"));
        QVERIFY(w->emptyText()->text().contains("ドラッグ"));
        // the button does what the toolbar does
        chosen = root("empty");
        QDir().mkpath(chosen);
        QTest::mouseClick(w->emptyAddButton(), Qt::LeftButton);
        QCOMPARE(Library(db).folders().size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(w->emptyKind(), K::NoRecordings, 5000);       // folder registered, nothing in it
        QVERIFY(w->emptyText()->text().contains("mp3"));
        QVERIFY(w->emptyAddButton()->isVisible());
        // a recording arrives: the cards replace the empty state
        QVERIFY(w->addFolder(root("rec")));
        QTRY_COMPARE_WITH_TIMEOUT(w->emptyKind(), K::None, 5000);
        QVERIFY(!w->emptyState()->isVisible());
        QVERIFY(w->cardView()->isVisible());
        // a search without hits is a different message and has no "add folder" button
        w->searchBox()->setText("zzzzqqq");
        w->runSearchNow();
        QCOMPARE(w->emptyKind(), K::NoMatches);
        QVERIFY(w->emptyTitle()->text().contains("該当"));
        QVERIFY(!w->emptyAddButton()->isVisible());
        w->searchBox()->clear();
        w->runSearchNow();
        QCOMPARE(w->emptyKind(), K::None);
    }
    void fileMenuHasTheFolderEntries()
    {
        start();
        QVERIFY(w->folderDialogAction());
        QCOMPARE(w->addFolderAction()->shortcut(), QKeySequence("Ctrl+O"));
        QCOMPARE(w->folderDialogAction()->shortcut(), QKeySequence("Ctrl+Shift+O"));
    }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    int rc = 0;
    { TstFolders t; rc |= QTest::qExec(&t, argc, argv); }
    { TstFoldersWindow t; rc |= QTest::qExec(&t, argc, argv); }
    return rc;
}
#include "tst_folders_ui.moc"
