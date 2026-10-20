#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QStatusBar>
#include <QtTest>

#include "core/Database.h"
#include "core/DemoData.h"
#include "ui/CardDelegate.h"
#include "ui/CardListView.h"
#include "ui/MainWindow.h"
#include "ui/RecordingListModel.h"

using namespace ake;
Q_DECLARE_METATYPE(ake::CardAction)

class TstCards : public QObject {
    Q_OBJECT
    Database db;

    QModelIndex indexOfTitle(RecordingListModel& m, const QString& title)
    {
        for (int r = 0; r < m.rowCount(); ++r)
            if (m.index(r, 0).data(RecordingListModel::TitleRole).toString() == title) return m.index(r, 0);
        return {};
    }

private slots:
    void initTestCase() { qRegisterMetaType<ake::CardAction>(); }
    void init()
    {
        QVERIFY(db.open(":memory:"));
        QVERIFY(seedDemoData(db));
    }

    // ---- model
    void aDoneRecordingWithoutAnyTextSaysThatNoSpeechWasFound()
    {
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("INSERT INTO recordings(id,folder_id,rel_path,abs_path,status,created_at,updated_at) VALUES(900,1,'noise.wav','/x/noise.wav','done',0,0)"));
        QVERIFY(q.exec("INSERT INTO transcripts(recording_id,language,model,created_at,is_current) VALUES(900,'','m',0,1)"));   // no segments
        QVERIFY(q.exec("INSERT INTO recordings(id,folder_id,rel_path,abs_path,status,created_at,updated_at) VALUES(901,1,'queued.wav','/x/queued.wav','queued',0,0)"));
        RecordingListModel m(db);
        const QModelIndex noise = m.index(m.rowForId(900), 0), queued = m.index(m.rowForId(901), 0);
        QVERIFY(noise.data(RecordingListModel::SummaryRole).toString().contains("音声が検出されませんでした"));
        QVERIFY(queued.data(RecordingListModel::SummaryRole).toString().isEmpty());          // not analysed yet: no claim
    }
    void modelPassesQtModelTester()
    {
        RecordingListModel m(db);
        QAbstractItemModelTester t(&m, QAbstractItemModelTester::FailureReportingMode::QtTest);
        m.reload();
        m.setSortOrder(RecordingListModel::SortOrder::Oldest);
        QCOMPARE(m.rowCount(), 9);
    }
    void modelJoinsAnalysesKeywordsAndStates()
    {
        RecordingListModel m(db);
        const QModelIndex docker = indexOfTitle(m, "Docker移行会議");
        QVERIFY(docker.isValid());
        QVERIFY(docker.data(RecordingListModel::SummaryRole).toString().contains("Podman"));
        QCOMPARE(docker.data(RecordingListModel::KeywordsRole).toStringList(), QStringList({ "docker", "linux", "podman" })); // by score
        QCOMPARE(docker.data(RecordingListModel::StateRole).value<CardState>(), CardState::Done);
        QCOMPARE(docker.data(RecordingListModel::FavoriteRole).toBool(), true);
        QCOMPARE(docker.data(RecordingListModel::DurationMsRole).toLongLong(), 2530 * 1000);

        // not analyzed -> falls back to file name, state by status
        const QModelIndex running = indexOfTitle(m, "interview_04.mp3");
        QCOMPARE(running.data(RecordingListModel::StateRole).value<CardState>(), CardState::Running);
        QVERIFY(qFuzzyCompare(running.data(RecordingListModel::ProgressRole).toDouble(), 0.62));
        const QModelIndex err = indexOfTitle(m, "broken_rec.flac");
        QCOMPARE(err.data(RecordingListModel::StateRole).value<CardState>(), CardState::Error);
        QVERIFY(err.data(RecordingListModel::ErrorRole).toString().contains("デコード"));
        const QModelIndex pend = indexOfTitle(m, "queued_note.wav");
        QCOMPARE(pend.data(RecordingListModel::StateRole).value<CardState>(), CardState::Pending);

        const auto c = m.counts();
        QCOMPARE(c.total, 9);
        QCOMPARE(c.done, 6);
        QCOMPARE(c.running, 1);
        QCOMPARE(c.error, 1);
        QCOMPARE(c.pending, 1);
    }
    void modelSortOrders()
    {
        RecordingListModel m(db);
        auto title = [&](int r) { return m.index(r, 0).data(RecordingListModel::TitleRole).toString(); };
        QCOMPARE(title(0), QString("Docker移行会議"));          // newest
        m.setSortOrder(RecordingListModel::SortOrder::Oldest);
        QCOMPARE(title(0), QString("queued_note.wav"));
        m.setSortOrder(RecordingListModel::SortOrder::Longest);
        QCOMPARE(title(0), QString("interview_04.mp3"));         // 4325 s
    }
    void modelReloadSeesNewData()
    {
        RecordingListModel m(db);
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("UPDATE recordings SET favorite=0, status='done' WHERE rel_path='broken/broken_rec.flac'"));
        QSignalSpy reset(&m, &QAbstractItemModel::modelReset);
        m.reload();
        QCOMPARE(reset.count(), 1);
        QCOMPARE(m.counts().error, 0);
    }

    // ---- delegate helpers
    void formatsDuration()
    {
        QCOMPARE(CardDelegate::formatDuration(0), QString("--:--"));
        QCOMPARE(CardDelegate::formatDuration(42 * 1000), QString("00:42"));
        QCOMPARE(CardDelegate::formatDuration((42 * 60 + 10) * 1000), QString("42:10"));
        QCOMPARE(CardDelegate::formatDuration((65 * 60 + 22) * 1000), QString("1:05:22"));
    }
    void wrapElideLimitsLinesAndAddsEllipsis()
    {
        const QFontMetrics fm{ QFont() };
        const QString longText(400, QChar(0x3042)); // あ x400
        const QStringList l = CardDelegate::wrapElide(longText, fm, 200, 3);
        QCOMPARE(l.size(), 3);
        QVERIFY(l.last().endsWith(QChar(0x2026)));
        for (const QString& line : l) QVERIFY(fm.horizontalAdvance(line) <= 200);
        // short text: no ellipsis, one line
        const QStringList s = CardDelegate::wrapElide("短い要約", fm, 200, 3);
        QCOMPARE(s, QStringList({ "短い要約" }));
        QVERIFY(CardDelegate::wrapElide("", fm, 200, 3).isEmpty());
        // Latin: words stay whole and no line starts with a space
        const QString en = "And so my fellow Americans, ask not what your country can do for you, ask what you can do for your country.";
        const QStringList e = CardDelegate::wrapElide(en, fm, 160, 10);
        QVERIFY(e.size() > 1);
        for (const QString& line : e) {
            QVERIFY2(!line.startsWith(' ') && !line.endsWith(' '), qPrintable("[" + line + "]"));
            QVERIFY(fm.horizontalAdvance(line) <= 160);
        }
        QCOMPARE(e.join(" "), en);                          // nothing lost, no word cut in two
        // a single over-long word still has to break somewhere
        QVERIFY(CardDelegate::wrapElide(QString(80, 'x'), fm, 100, 5).size() > 1);
    }

    // ---- view behaviour
    void clickingPlayRetryAnalyzeEmitsActions()
    {
        MainWindow w;
        w.setDatabase(&db);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        CardListView* v = w.cardView();
        auto* m = w.recordingModel();
        QSignalSpy spy(v, &CardListView::cardActionRequested);

        auto click = [&](const QModelIndex& idx, CardAction a) {
            v->scrollTo(idx);
            const QRect btn = CardDelegate::actionButtonRect(v->visualRect(idx), a);
            QTest::mouseMove(v->viewport(), btn.center());
            QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, btn.center());
        };
        const QModelIndex docker = indexOfTitle(*m, "Docker移行会議");
        click(docker, CardAction::Play);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.last().at(0).toLongLong(), docker.data(RecordingListModel::IdRole).toLongLong());
        QCOMPARE(spy.last().at(1).value<CardAction>(), CardAction::Play);

        click(indexOfTitle(*m, "broken_rec.flac"), CardAction::Retry);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.last().at(1).value<CardAction>(), CardAction::Retry);
        // a queued card offers no button; a not-yet-analysed ('new') one offers "解析する"
        CardAction none;
        QVERIFY(!CardDelegate::actionFor(indexOfTitle(*m, "queued_note.wav"), &none));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("UPDATE recordings SET status='new' WHERE rel_path='memos/queued_note.wav'"));
        m->reload();
        click(indexOfTitle(*m, "queued_note.wav"), CardAction::Analyze);
        QCOMPARE(spy.count(), 3);
        QCOMPARE(spy.last().at(1).value<CardAction>(), CardAction::Analyze);

        // clicking the card body (not a button) must not emit an action
        const QModelIndex podman = indexOfTitle(*m, "Podman勉強会 #3");
        v->scrollTo(podman);
        QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, v->visualRect(podman).center() - QPoint(40, 20));
        QCOMPARE(spy.count(), 3);
        // running cards offer no action
        CardAction dummy;
        QVERIFY(!CardDelegate::actionFor(indexOfTitle(*m, "interview_04.mp3"), &dummy));
    }
    void selectionEmitsRecordingId()
    {
        MainWindow w;
        w.setDatabase(&db);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        CardListView* v = w.cardView();
        QSignalSpy spy(v, &CardListView::currentRecordingChanged);
        const QModelIndex idx = indexOfTitle(*w.recordingModel(), "技術ポッドキャスト Ep.87");
        QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, v->visualRect(idx).center());
        QVERIFY(spy.count() >= 1);
        QCOMPARE(spy.last().at(0).toLongLong(), idx.data(RecordingListModel::IdRole).toLongLong());
    }
    void statusBarShowsCounts()
    {
        MainWindow w;
        w.setDatabase(&db);
        QCOMPARE(w.statusBar()->currentMessage(), QString("9件 ・ 解析済 6 ・ 解析中 1 ・ 待機 1 ・ エラー 1"));
    }
    void selectedCardBorderUsesAccentColor()
    {
        MainWindow w;
        w.applyTheme(Theme::light());
        w.setDatabase(&db);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        CardListView* v = w.cardView();
        const QModelIndex idx = w.recordingModel()->index(0, 0);
        v->setCurrentIndex(idx);
        QTest::qWait(50);
        const QImage img = v->viewport()->grab().toImage();
        const QRect c = CardDelegate::cardRect(v->visualRect(idx));
        QCOMPARE(img.pixelColor(c.left() + 1, c.center().y()), Theme::light().accent);
    }
    void emptyDatabaseShowsZero()
    {
        Database empty;
        QVERIFY(empty.open(":memory:"));
        MainWindow w;
        w.setDatabase(&empty);
        QCOMPARE(w.recordingModel()->rowCount(), 0);
        QCOMPARE(w.statusBar()->currentMessage(), QString("0件 ・ 解析済 0 ・ 解析中 0 ・ 待機 0 ・ エラー 0"));
    }
};

QTEST_MAIN(TstCards)
#include "tst_cards.moc"
