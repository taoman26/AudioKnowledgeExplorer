#include <QAction>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QToolButton>
#include <QtTest>

#include "core/Database.h"
#include "core/ITokenizer.h"
#include "core/JobScheduler.h"
#include "core/KnowledgeBuilder.h"
#include "core/RecordingDetail.h"
#include "core/SearchService.h"
#include "core/SqlText.h"
#include "core/TranscriptRepository.h"
#include "ui/CardListView.h"
#include "ui/DetailPane.h"
#include "ui/MainWindow.h"
#include "ui/RecordingListModel.h"
#include "ui/SidebarWidget.h"
#include "Fakes.h"

using namespace ake;
using namespace fakes;

class TstDetailUi : public QObject {
    Q_OBJECT
    Database db;
    FakeDecoder dec;
    std::unique_ptr<ITokenizer> tok;
    std::unique_ptr<JobScheduler> sched;
    std::unique_ptr<MainWindow> w;
    qint64 docker = 0, memo = 0, pending = 0, broken = 0;
    int m_n = 0;

    qint64 add(const QString& relPath, const QStringList& segments, const QString& status = "done", const QString& err = QString())
    {
        QSqlQuery q(db.handle());
        q.prepare("INSERT INTO recordings(folder_id,rel_path,abs_path,status,error_msg,recorded_at,created_at,updated_at,duration_ms) VALUES(1,?,?,?,?,?,0,0,125000)");
        q.addBindValue(relPath); q.addBindValue("/f/" + relPath); q.addBindValue(status); q.addBindValue(sqlText(err)); q.addBindValue(1700000000 + m_n++ * 100);
        if (!q.exec()) qFatal("%s", qPrintable(q.lastError().text()));
        const qint64 id = q.lastInsertId().toLongLong();
        if (!segments.isEmpty()) {
            TranscriptResult r; r.language = "ja"; r.model = "ggml-small";
            qint64 t = 0;
            for (const QString& s : segments) { r.segments.append({ t, t + 3000, s }); t += 3000; }
            if (!TranscriptRepository(db).save(id, r)) qFatal("save");
            if (!KnowledgeBuilder(db, *tok).build(id)) qFatal("build");
        }
        return id;
    }
    static QStringList meetingText()
    {
        QStringList s;
        for (int i = 0; i < 3; ++i)
            s << "Podmanへの移行方針について議論しました。" << "rootlessの運用とsystemd連携を確認しました。" << "共有フォルダの権限管理も検討しました。";
        return s;
    }
    DetailPane* pane() { return w->detailPaneWidget(); }
    RecordingListModel* model() { return w->recordingModel(); }
    void select(qint64 id)
    {
        const int row = model()->rowForId(id);
        QVERIFY(row >= 0);
        w->cardView()->setCurrentIndex(model()->index(row, 0));
        QCOMPARE(pane()->currentId(), id);
    }
    QString cardTitle(qint64 id) { return model()->index(model()->rowForId(id), 0).data(RecordingListModel::TitleRole).toString(); }
    RecordingDetail detail(qint64 id) { return RecordingDetail::load(db, id); }
    void startWindow(bool withTokenizer = true, bool withScheduler = false)
    {
        sched = std::make_unique<JobScheduler>(db, dec, factory());
        sched->setTokenizer(tok.get());
        w = std::make_unique<MainWindow>();
        if (withTokenizer) w->setTokenizer(tok.get());
        w->setDatabase(&db);
        if (withScheduler) w->setScheduler(sched.get());
        w->show();
        QVERIFY(QTest::qWaitForWindowActive(w.get()));
    }

private slots:
    void init()
    {
        tok = createTokenizer();
        QVERIFY(db.open(":memory:"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("INSERT INTO folders(id,path) VALUES(1,'/f')"));
        m_n = 0;
        g_stepMs = 5;
        docker = add("meetings/docker.m4a", meetingText());
        memo = add("memos/shopping.wav", { "牛乳とトマトを買って帰ります。今日は天気がよいですね。", "週末は家族で出かけます。" });
        pending = add("incoming/new_file.mp3", {}, "new");
        broken = add("incoming/broken.flac", {}, "error", "デコードに失敗しました");
    }
    void cleanup() { w.reset(); sched.reset(); }

    // ---- showing
    void showsPlaceholderUntilSomethingIsSelected()
    {
        startWindow();
        QVERIFY(!pane()->showsRecording());
        select(docker);
        QVERIFY(pane()->showsRecording());
        w->cardView()->setCurrentIndex(QModelIndex());
        QCOMPARE(pane()->currentId(), qint64(0));
        QVERIFY(!pane()->showsRecording());
    }
    void showsTheSelectedRecordingsDetails()
    {
        startWindow();
        select(docker);
        const RecordingDetail d = detail(docker);
        QCOMPARE(pane()->titleText(), d.title.content);
        QCOMPARE(pane()->titleText(), cardTitle(docker));
        QCOMPARE(pane()->badgeText(true), QString("AI生成"));
        QCOMPARE(pane()->summaryText(), d.summary.content);
        QCOMPARE(pane()->badgeText(false), QString("AI生成"));
        QStringList kws;
        for (const auto& k : d.keywords) kws << k.name;
        QCOMPARE(pane()->keywordTexts(), kws);
        for (const QString& k : kws) QVERIFY2(pane()->removeButtonOfKeyword(k), qPrintable(k));
        const QString meta = pane()->metaText();
        QVERIFY2(meta.contains("2023-11-1") && meta.contains("02:05") && meta.contains("ja") && meta.contains("解析済"), qPrintable(meta));
        QVERIFY(pane()->openButton()->isEnabled());
        QVERIFY(pane()->favoriteButton()->isEnabled());
        QCOMPARE(pane()->favoriteButton()->text(), QString("☆"));
        select(memo);
        QCOMPARE(pane()->titleText(), cardTitle(memo));                  // switching updates everything
        QVERIFY(pane()->keywordTexts() != kws);
    }
    void notYetAnalysedAndFailedRecordingsExplainThemselves()
    {
        startWindow();
        select(pending);
        QCOMPARE(pane()->titleText(), QString("new_file.mp3"));         // no AI title: the file name
        QVERIFY(pane()->badgeText(true).isEmpty());
        QVERIFY2(pane()->summaryText().contains("解析"), qPrintable(pane()->summaryText()));
        QVERIFY(!pane()->openButton()->isEnabled());                     // no transcript to open
        QVERIFY(!pane()->summaryEditButton()->isEnabled());
        QVERIFY(pane()->metaText().contains("未解析"));
        select(broken);
        QCOMPARE(pane()->summaryText(), QString("デコードに失敗しました"));
        QVERIFY(pane()->metaText().contains("エラー"));
    }

    // ---- title
    void editingTheTitleSavesAUserTitleEverywhere()
    {
        startWindow();
        select(docker);
        const QString ai = pane()->titleText();
        QSignalSpy edited(pane(), &DetailPane::recordingEdited);
        QTest::mouseClick(pane()->titleEditButton(), Qt::LeftButton);
        QVERIFY(!pane()->titleEdit()->isHidden());
        QCOMPARE(pane()->titleEdit()->text(), ai);
        QCOMPARE(pane()->titleEdit()->selectedText(), ai);               // ready to be replaced
        pane()->titleEdit()->setText("週次スタンドアップ");
        QTest::keyClick(pane()->titleEdit(), Qt::Key_Return);
        QVERIFY(pane()->titleEdit()->isHidden());
        QCOMPARE(pane()->titleText(), QString("週次スタンドアップ"));
        QCOMPARE(pane()->badgeText(true), QString("編集済み"));
        QVERIFY(!pane()->titleRevertButton()->isHidden());
        QCOMPARE(cardTitle(docker), QString("週次スタンドアップ"));        // the card follows
        QVERIFY(detail(docker).title.isUser);
        QVERIFY(edited.count() >= 1);
        SearchService svc(db, *tok);
        QCOMPARE(svc.search("スタンドアップ").hits.size(), 1);             // searchable right away
    }
    void doubleClickingTheTitleAlsoEdits()
    {
        startWindow();
        select(docker);
        QLabel* title = pane()->findChild<QLabel*>("detailTitle");
        QVERIFY(title);
        QVERIFY(title->isVisible());
        QTest::mouseDClick(title, Qt::LeftButton);
        QTRY_VERIFY(!pane()->titleEdit()->isHidden());          // starts right after the click sequence ends
        QVERIFY(title->isHidden());
    }
    void escapeAndUnchangedTextLeaveTheAiTitleAlone()
    {
        startWindow();
        select(docker);
        QTest::mouseClick(pane()->titleEditButton(), Qt::LeftButton);
        pane()->titleEdit()->setText("捨てる変更");
        QTest::keyClick(pane()->titleEdit(), Qt::Key_Escape);
        QVERIFY(pane()->titleEdit()->isHidden());
        QVERIFY(!detail(docker).title.isUser);
        // open and confirm without changing anything: must not turn the AI title into a user title
        QTest::mouseClick(pane()->titleEditButton(), Qt::LeftButton);
        QTest::keyClick(pane()->titleEdit(), Qt::Key_Return);
        QVERIFY(!detail(docker).title.isUser);
        QCOMPARE(pane()->badgeText(true), QString("AI生成"));
    }
    void revertAndEmptyingBringTheAiTitleBack()
    {
        startWindow();
        select(docker);
        const QString ai = pane()->titleText();
        QTest::mouseClick(pane()->titleEditButton(), Qt::LeftButton);
        pane()->titleEdit()->setText("手書き");
        QTest::keyClick(pane()->titleEdit(), Qt::Key_Return);
        QCOMPARE(pane()->titleText(), QString("手書き"));
        QTest::mouseClick(pane()->titleRevertButton(), Qt::LeftButton);
        QCOMPARE(pane()->titleText(), ai);
        QVERIFY(pane()->titleRevertButton()->isHidden());
        QCOMPARE(cardTitle(docker), ai);
        QTest::mouseClick(pane()->titleEditButton(), Qt::LeftButton);        // emptying the field = revert as well
        pane()->titleEdit()->setText("別の手書き");
        QTest::keyClick(pane()->titleEdit(), Qt::Key_Return);
        QTest::mouseClick(pane()->titleEditButton(), Qt::LeftButton);
        pane()->titleEdit()->setText("   ");
        QTest::keyClick(pane()->titleEdit(), Qt::Key_Return);
        QCOMPARE(pane()->titleText(), ai);
    }

    // ---- summary
    void editingTheSummaryWorksAndCanBeReverted()
    {
        startWindow();
        select(docker);
        const QString ai = pane()->summaryText();
        QTest::mouseClick(pane()->summaryEditButton(), Qt::LeftButton);
        QVERIFY(pane()->summaryEdit()->isVisible());
        QCOMPARE(pane()->summaryEdit()->toPlainText(), ai);
        pane()->summaryEdit()->setPlainText("要点:\n- 段階移行\n- 担当を決める");
        QTest::mouseClick(pane()->summarySaveButton(), Qt::LeftButton);
        QVERIFY(!pane()->summaryEdit()->isVisible());
        QVERIFY(pane()->summaryText().contains("段階移行"));
        QCOMPARE(pane()->badgeText(false), QString("編集済み"));
        QVERIFY(!pane()->summaryRevertButton()->isHidden());
        QCOMPARE(model()->index(model()->rowForId(docker), 0).data(RecordingListModel::SummaryRole).toString().contains("段階移行"), true);
        QTest::mouseClick(pane()->summaryRevertButton(), Qt::LeftButton);
        QCOMPARE(pane()->summaryText(), ai);
        // Ctrl+Return saves, Escape cancels
        QTest::mouseClick(pane()->summaryEditButton(), Qt::LeftButton);
        pane()->summaryEdit()->setPlainText("キーボードで保存");
        QTest::keyClick(pane()->summaryEdit(), Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(pane()->summaryText(), QString("キーボードで保存"));
        QTest::mouseClick(pane()->summaryEditButton(), Qt::LeftButton);
        pane()->summaryEdit()->setPlainText("これは捨てる");
        QTest::keyClick(pane()->summaryEdit(), Qt::Key_Escape);
        QCOMPARE(pane()->summaryText(), QString("キーボードで保存"));
    }

    // ---- favorite
    void starTogglesFavoriteOnCardAndSidebar()
    {
        startWindow();
        select(docker);
        const auto favCount = [&] { return w->sidebarWidget()->findItem(SidebarWidget::Library, int(RecordingFilter::Scope::Favorites))->data(0, SidebarWidget::CountRole).toInt(); };
        QCOMPARE(favCount(), 0);
        QTest::mouseClick(pane()->favoriteButton(), Qt::LeftButton);
        QCOMPARE(pane()->favoriteButton()->text(), QString("★"));
        QVERIFY(model()->index(model()->rowForId(docker), 0).data(RecordingListModel::FavoriteRole).toBool());
        QCOMPARE(favCount(), 1);
        QTest::mouseClick(pane()->favoriteButton(), Qt::LeftButton);
        QCOMPARE(pane()->favoriteButton()->text(), QString("☆"));
        QCOMPARE(favCount(), 0);
    }

    // ---- keywords
    void addingAKeywordPutsItFirstOnTheCardAndInTheSidebar()
    {
        startWindow();
        select(docker);
        const int before = pane()->keywordTexts().size();
        QTest::mouseClick(pane()->addKeywordButton(), Qt::LeftButton);
        QVERIFY(pane()->keywordEdit()->isVisible());
        pane()->keywordEdit()->setText("重要案件");                       // (QTest::keyClicks only handles ASCII)
        QTest::keyClick(pane()->keywordEdit(), Qt::Key_Return);
        QVERIFY(!pane()->keywordEdit()->isVisible());
        QCOMPARE(pane()->keywordTexts().size(), before + 1);
        QCOMPARE(pane()->keywordTexts().first(), QString("重要案件"));
        QCOMPARE(model()->index(model()->rowForId(docker), 0).data(RecordingListModel::KeywordsRole).toStringList().first(), QString("重要案件"));
        bool inSidebar = false;
        for (const auto& t : w->sidebarWidget()->data().tags) inSidebar |= t.name == "重要案件";
        QVERIFY(inSidebar);
        // Escape cancels without adding
        QTest::mouseClick(pane()->addKeywordButton(), Qt::LeftButton);
        pane()->keywordEdit()->setText("やめた");
        QTest::keyClick(pane()->keywordEdit(), Qt::Key_Escape);
        QVERIFY(!pane()->keywordEdit()->isVisible());
        QCOMPARE(pane()->keywordTexts().size(), before + 1);
        QVERIFY(pane()->addKeywordButton()->isVisible());
    }
    void removingAKeywordKeepsItRemovedEvenAfterReanalysis()
    {
        startWindow();
        select(docker);
        const QStringList before = pane()->keywordTexts();
        QVERIFY(before.size() >= 3);
        const QString victim = before[1];
        QTest::mouseClick(pane()->removeButtonOfKeyword(victim), Qt::LeftButton);
        QVERIFY(!pane()->keywordTexts().contains(victim));
        QVERIFY(!model()->index(model()->rowForId(docker), 0).data(RecordingListModel::KeywordsRole).toStringList().contains(victim));
        QVERIFY(KnowledgeBuilder(db, *tok).build(docker));               // analysed again
        pane()->reload();
        QVERIFY(!pane()->keywordTexts().contains(victim));
        QCOMPARE(pane()->keywordTexts().size(), before.size());         // refilled with the next candidate
    }

    // ---- memo
    void memoIsSavedAfterTypingPausesAndIsSearchable()
    {
        startWindow();
        select(docker);
        pane()->memoEdit()->setPlainText("次回はQuadletを試す");
        QCOMPARE(detail(docker).notes, QString());                        // not yet: still typing
        QTRY_COMPARE_WITH_TIMEOUT(detail(docker).notes, QString("次回はQuadletを試す"), DetailPane::kMemoSaveDelayMs + 1500);
        SearchService svc(db, *tok);
        QCOMPARE(svc.search("Quadlet").hits.size(), 1);
    }
    void switchingRecordingsNeverLosesAPendingMemo()
    {
        startWindow();
        select(docker);
        pane()->memoEdit()->setPlainText("保存されるべきメモ");
        select(memo);                                                       // before the debounce fires
        QCOMPARE(detail(docker).notes, QString("保存されるべきメモ"));
        QVERIFY(pane()->memoText().isEmpty());                              // the other recording has its own (empty) memo
        select(docker);
        QCOMPARE(pane()->memoText(), QString("保存されるべきメモ"));
    }
    void clearingTheMemoDeletesIt()
    {
        startWindow();
        select(docker);
        pane()->memoEdit()->setPlainText("一時的なメモ");
        pane()->saveMemoNow();
        QCOMPARE(detail(docker).notes, QString("一時的なメモ"));
        pane()->memoEdit()->clear();
        pane()->saveMemoNow();
        QCOMPARE(detail(docker).notes, QString());
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("SELECT count(*) FROM notes"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 0);
    }

    // ---- actions
    void buttonsAskTheWindowToOpenPlayAndReanalyse()
    {
        startWindow(true, true);
        select(docker);
        QSignalSpy open(w.get(), &MainWindow::openTranscriptRequested), play(w.get(), &MainWindow::playRequested);
        QTest::mouseClick(pane()->openButton(), Qt::LeftButton);
        QCOMPARE(open.count(), 1);
        QCOMPARE(open.at(0).at(0).toLongLong(), docker);
        QCOMPARE(open.at(0).at(1).toLongLong(), qint64(0));
        QTest::mouseClick(pane()->playButton(), Qt::LeftButton);
        QCOMPARE(play.count(), 1);
        QCOMPARE(play.at(0).at(0).toLongLong(), docker);
        QCOMPARE(pane()->reanalyzeButton()->text(), QString("再解析"));
        QTest::mouseClick(pane()->reanalyzeButton(), Qt::LeftButton);
        QVERIFY(sched->isBusy());
        QTRY_VERIFY_WITH_TIMEOUT(!sched->isBusy(), 10000);
        QCOMPARE(detail(docker).status, QString("done"));
        QVERIFY(pane()->metaText().contains("解析済"));
    }
    void analysingFromThePaneUpdatesItWhenTheJobFinishes()
    {
        startWindow(true, true);
        select(pending);
        QCOMPARE(pane()->reanalyzeButton()->text(), QString("解析する"));
        QVERIFY(pane()->reanalyzeButton()->isEnabled());
        g_stepMs = 20;
        QTest::mouseClick(pane()->reanalyzeButton(), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(pane()->metaText().contains("解析中"), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(pane()->metaText().contains("解析済"), 10000);   // no manual refresh needed
        QVERIFY(pane()->openButton()->isEnabled());
        QVERIFY(!pane()->summaryText().isEmpty());
        QCOMPARE(pane()->badgeText(false), QString("AI生成"));
    }
    void withoutAnySchedulerReanalysisIsDisabled()
    {
        startWindow(true, false);
        select(docker);
        QVERIFY(!pane()->reanalyzeButton()->isEnabled());
    }

    // ---- robustness
    void readOnlyWithoutATokenizer()
    {
        startWindow(false);
        select(docker);
        QVERIFY(pane()->showsRecording());
        QVERIFY(!pane()->favoriteButton()->isEnabled());
        QVERIFY(!pane()->titleEditButton()->isEnabled());
        QVERIFY(!pane()->summaryEditButton()->isEnabled());
        QVERIFY(!pane()->addKeywordButton()->isEnabled());
        QVERIFY(pane()->memoEdit()->isReadOnly());
        QVERIFY(pane()->openButton()->isEnabled());                         // viewing and navigation still work
    }
    void deletingTheSelectedRecordingFallsBackToThePlaceholder()
    {
        startWindow();
        select(memo);
        QSqlQuery q(db.handle());
        QVERIFY(q.exec(QString("DELETE FROM recordings WHERE id=%1").arg(memo)));
        pane()->reload();
        QVERIFY(!pane()->showsRecording());
        QCOMPARE(pane()->currentId(), qint64(0));
    }
    void longPathsAreElidedWithTheFullPathInTheTooltip()
    {
        const QString longPath = "very/deep/folder/structure/with/many/levels/that/go/on/and/on/forever/and/ever/recording_with_a_long_name_2026.m4a";
        const qint64 id = add(longPath, { "短い録音の内容について話しました。" });
        startWindow();
        w->resize(1200, 800);
        select(id);
        const QLabel* path = pane()->findChild<QLabel*>("detailSub");
        QVERIFY(path);
        QVERIFY(path->text().contains(QChar(0x2026)));
        QCOMPARE(path->toolTip(), "/f/" + longPath);
    }
    void switchingBetweenManyRecordingsDoesNotLeakEditorState()
    {
        startWindow();
        select(docker);
        QTest::mouseClick(pane()->titleEditButton(), Qt::LeftButton);
        QVERIFY(!pane()->titleEdit()->isHidden());
        select(memo);                                                        // leaving while editing the title
        QVERIFY(pane()->titleEdit()->isHidden());
        QVERIFY(!detail(docker).title.isUser);
        QCOMPARE(pane()->titleText(), cardTitle(memo));
    }
};

QTEST_MAIN(TstDetailUi)
#include "tst_detail_ui.moc"
