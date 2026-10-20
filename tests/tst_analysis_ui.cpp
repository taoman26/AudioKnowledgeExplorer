#include <QAction>
#include <QLabel>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QtTest>

#include "core/Database.h"
#include "core/JobScheduler.h"
#include "core/Settings.h"
#include "core/SqlText.h"
#include "ui/CardListView.h"
#include "ui/MainWindow.h"
#include "ui/QueueStatusWidget.h"
#include "ui/RecordingListModel.h"
#include "ui/SidebarWidget.h"
#include "Fakes.h"

using namespace ake;
using namespace fakes;
using Scope = RecordingFilter::Scope;

class TstAnalysisUi : public QObject {
    Q_OBJECT
    Database db;
    FakeDecoder dec;
    std::unique_ptr<JobScheduler> sched;
    std::unique_ptr<MainWindow> w;

    qint64 add(const QString& name, const QString& status = "new", const QString& err = QString())
    {
        QSqlQuery q(db.handle());
        q.prepare("INSERT INTO recordings(folder_id,rel_path,abs_path,status,error_msg,recorded_at,created_at,updated_at)"
                  " VALUES(1,?,?,?,?,?,0,0)");
        q.addBindValue(name); q.addBindValue("/f/" + name); q.addBindValue(status); q.addBindValue(sqlText(err));
        q.addBindValue(1000000 + m_n++);
        if (!q.exec()) qFatal("%s", qPrintable(q.lastError().text()));
        return q.lastInsertId().toLongLong();
    }
    int m_n = 0;
    void start()
    {
        sched = std::make_unique<JobScheduler>(db, dec, factory());
        w = std::make_unique<MainWindow>();
        w->setDatabase(&db);
        w->setScheduler(sched.get());
        w->show();
        QVERIFY(QTest::qWaitForWindowExposed(w.get()));
    }
    QModelIndex idx(qint64 id) { return w->recordingModel()->index(w->recordingModel()->rowForId(id), 0); }
    CardState state(qint64 id) { return idx(id).data(RecordingListModel::StateRole).value<CardState>(); }
    void clickButton(qint64 id, CardAction a)
    {
        CardListView* v = w->cardView();
        v->scrollTo(idx(id));
        const QPoint c = CardDelegate::actionButtonRect(v->visualRect(idx(id)), a).center();
        QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, c);
    }
    QString dbStatus(qint64 id)
    {
        QSqlQuery q(db.handle());
        q.exec(QString("SELECT status FROM recordings WHERE id=%1").arg(id));
        return q.next() ? q.value(0).toString() : QString();
    }
    int sidebarCount(Scope s)
    {
        return w->sidebarWidget()->findItem(SidebarWidget::Library, int(s))->data(0, SidebarWidget::CountRole).toInt();
    }

private slots:
    void init()
    {
        QVERIFY(db.open(":memory:"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("INSERT INTO folders(id,path) VALUES(1,'/f')"));
        g_stepMs = 5; g_running = 0; g_maxRunning = 0;
        m_n = 0;
    }
    void cleanup() { w.reset(); sched.reset(); }

    void analyzeButtonRunsJobAndCardTurnsDone()
    {
        g_stepMs = 25;
        const qint64 id = add("meeting.m4a");
        start();
        QCOMPARE(state(id), CardState::Pending);
        QCOMPARE(sidebarCount(Scope::Unanalyzed), 1);
        QSignalSpy reset(w->recordingModel(), &QAbstractItemModel::modelReset);

        clickButton(id, CardAction::Analyze);
        // running: the card shows live progress and the status bar widget appears with the title
        QTRY_COMPARE_WITH_TIMEOUT(state(id), CardState::Running, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(idx(id).data(RecordingListModel::ProgressRole).toDouble() > 0.1, 3000);
        QVERIFY(!idx(id).data(RecordingListModel::StageRole).toString().isEmpty());
        QVERIFY(w->queueStatus()->isVisible());
        QVERIFY2(w->queueStatus()->text().contains("meeting.m4a"), qPrintable(w->queueStatus()->text()));
        QVERIFY(w->queueStatus()->text().contains("%"));
        QVERIFY(w->queueStatus()->percent() > 0);

        QTRY_COMPARE_WITH_TIMEOUT(state(id), CardState::Done, 8000);
        QCOMPARE(dbStatus(id), QString("done"));
        QTRY_VERIFY_WITH_TIMEOUT(!w->queueStatus()->isVisible(), 2000);
        QCOMPARE(sidebarCount(Scope::Unanalyzed), 0);
        QCOMPARE(reset.count(), 0); // rows were updated in place, never rebuilt
        QVERIFY(w->statusBar()->currentMessage().contains("解析済 1"));
    }
    void retryButtonReRunsFailedRecording()
    {
        const qint64 bad = add("undecodable.mp3", "error", "cannot open: broken file");
        start();
        QCOMPARE(state(bad), CardState::Error);
        clickButton(bad, CardAction::Retry);
        // still undecodable: fails again, but it really did run (error text is refreshed, not stale)
        QTRY_COMPARE_WITH_TIMEOUT(dbStatus(bad), QString("error"), 3000);
        QTRY_VERIFY(!sched->isBusy());
        QCOMPARE(idx(bad).data(RecordingListModel::ErrorRole).toString(), QString("cannot open: broken file"));
        QCOMPARE(state(bad), CardState::Error);
        QCOMPARE(sidebarCount(Scope::Error), 1);
    }
    void analyzeAllMenuQueuesNewOnesOnly()
    {
        g_stepMs = 20;
        const qint64 a = add("a.mp3"), b = add("b.mp3"), c = add("c.mp3"), done = add("d.mp3", "done");
        start();
        w->analyzeAllAction()->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(w->queueStatus()->text().contains("待機 2"), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!sched->isBusy(), 20000);
        for (qint64 id : { a, b, c, done }) QCOMPARE(state(id), CardState::Done);
        QCOMPARE(sidebarCount(Scope::Unanalyzed), 0);
        QCOMPARE(g_maxRunning.load(), 1);
    }
    void pauseActionHoldsQueueAndShowsIt()
    {
        g_stepMs = 15;
        add("a.mp3"); const qint64 b = add("b.mp3");
        start();
        w->pauseAction()->setChecked(true);
        w->analyzeAllAction()->trigger();           // paused before anything started: nothing may run
        QTest::qWait(300);
        QCOMPARE(sched->runningId(), qint64(0));
        QCOMPARE(sched->queuedCount(), 2);
        QVERIFY2(w->queueStatus()->text().contains("一時停止中"), qPrintable(w->queueStatus()->text()));
        QVERIFY(w->queueStatus()->text().contains("待機 2"));
        QCOMPARE(state(b), CardState::Pending); // queued
        w->pauseAction()->setChecked(false);
        QTRY_VERIFY_WITH_TIMEOUT(!sched->isBusy(), 20000);
        QCOMPARE(state(b), CardState::Done);
        QVERIFY(!w->queueStatus()->isVisible());
    }
    void cancelAllMenuStopsEverythingAndResetsCards()
    {
        g_stepMs = 40;
        const qint64 a = add("a.mp3"), b = add("b.mp3");
        start();
        w->analyzeAllAction()->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(state(a), CardState::Running, 2000);
        w->cancelAllAction()->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!sched->isBusy(), 5000);
        QCOMPARE(dbStatus(a), QString("new"));
        QCOMPARE(dbStatus(b), QString("new"));
        QVERIFY(!w->queueStatus()->isVisible());
    }
    void selectionSurvivesJobUpdates()
    {
        g_stepMs = 20;
        const qint64 sel = add("selected.mp3", "done"), job = add("job.mp3");
        start();
        w->cardView()->setCurrentIndex(idx(sel));
        QCOMPARE(w->cardView()->currentIndex().data(RecordingListModel::IdRole).toLongLong(), sel);
        QSignalSpy reset(w->recordingModel(), &QAbstractItemModel::modelReset);
        sched->enqueue(job);
        QTRY_VERIFY_WITH_TIMEOUT(!sched->isBusy(), 10000);
        QCOMPARE(w->cardView()->currentIndex().data(RecordingListModel::IdRole).toLongLong(), sel);
        QCOMPARE(reset.count(), 0);
    }
    void finishedJobLeavesAnActiveFilterWhenItNoLongerMatches()
    {
        const qint64 a = add("a.mp3"), b = add("b.mp3");
        start();
        w->sidebarWidget()->setFilter([] { RecordingFilter f; f.scope = Scope::Unanalyzed; return f; }());
        QCOMPARE(w->recordingModel()->rowCount(), 2);
        sched->enqueue(a);
        QTRY_VERIFY_WITH_TIMEOUT(!sched->isBusy(), 10000);
        QCOMPARE(w->recordingModel()->rowCount(), 1);            // a is done now -> removed from "未解析"
        QCOMPARE(w->recordingModel()->rowForId(a), -1);
        QVERIFY(w->recordingModel()->rowForId(b) >= 0);
        QCOMPARE(sidebarCount(Scope::Unanalyzed), 1);
    }
    void interruptedJobsResumeOnStartup()
    {
        const qint64 a = add("a.mp3", "transcribing"), b = add("b.mp3", "queued");
        start();
        QCOMPARE(sched->resumeInterrupted(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(!sched->isBusy(), 10000);
        QCOMPARE(state(a), CardState::Done);
        QCOMPARE(state(b), CardState::Done);
    }

    void missingModelShowsBannerUntilItExists()
    {
        QTemporaryDir dir;
        const QString model = dir.filePath("ggml-test.bin");
        start();
        Settings(db).set(Settings::kModelPath, model);   // applySettings() reads the stored value
        QWidget* banner = w->findChild<QWidget*>("modelBanner");
        QVERIFY(banner);
        w->applySettings();
        QVERIFY(!banner->isVisible());              // no recording yet: the empty state leads the way

        add("a.mp3");
        w->applySettings();
        QVERIFY(banner->isVisible());
        QVERIFY(banner->findChild<QLabel*>()->text().contains(model));

        QFile f(model);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        w->applySettings();
        QVERIFY(!banner->isVisible());
    }
};

QTEST_MAIN(TstAnalysisUi)
#include "tst_analysis_ui.moc"
