#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QToolBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QToolButton>
#include <QtTest>

#include "core/AnalysisRepository.h"
#include "core/Database.h"
#include "core/ITokenizer.h"
#include "core/KnowledgeBuilder.h"
#include "core/RecordingDetail.h"
#include "core/SearchService.h"
#include "core/TranscriptRepository.h"
#include "ui/CardListView.h"
#include "ui/DetailPane.h"
#include "ui/MainWindow.h"
#include "ui/RecordingListModel.h"
#include "ui/SearchSuggestPopup.h"
#include "ui/TranscriptModel.h"
#include "ui/TranscriptView.h"

using namespace ake;

class TstTranscriptUi : public QObject {
    Q_OBJECT
    Database db;
    std::unique_ptr<ITokenizer> tok;
    qint64 docker = 0, memo = 0;
    int m_n = 0;

    static QStringList meetingText()
    {
        return { "今日はDocker ComposeからPodmanへの移行方針について議論します。",   // 0
                 "Podmanはrootlessで動くので、セキュリティ面のメリットが大きいです。", // 1
                 "共有フォルダの権限管理も検討しました。",                              // 2
                 "来週までに検証担当者を決めます。",                                    // 3
                 "ＰＯＤＭＡＮの全角表記もここにあります。",                            // 4 (full-width)
                 "最後に、podman と Docker の違いをまとめます。" };                   // 5
    }
    qint64 add(const QString& relPath, const QStringList& segments, qint64 stepMs = 4000)
    {
        QSqlQuery q(db.handle());
        q.prepare("INSERT INTO recordings(folder_id,rel_path,abs_path,status,recorded_at,created_at,updated_at,duration_ms) VALUES(1,?,?,'done',?,0,0,?)");
        q.addBindValue(relPath); q.addBindValue("/f/" + relPath); q.addBindValue(1700000000 + m_n++ * 100); q.addBindValue(stepMs * segments.size());
        if (!q.exec()) qFatal("%s", qPrintable(q.lastError().text()));
        const qint64 id = q.lastInsertId().toLongLong();
        TranscriptResult r; r.language = "ja"; r.model = "ggml-small";
        qint64 t = 0;
        for (const QString& s : segments) { r.segments.append({ t, t + stepMs - 200, s }); t += stepMs; }
        if (!TranscriptRepository(db).save(id, r)) qFatal("save");
        if (!KnowledgeBuilder(db, *tok).build(id)) qFatal("build");
        return id;
    }
    std::unique_ptr<TranscriptView> view;
    void openView(qint64 id, qint64 ms = -1, const QString& query = QString(), bool withTokenizer = true)
    {
        view = std::make_unique<TranscriptView>();
        view->setDatabase(&db, withTokenizer ? tok.get() : nullptr);
        view->resize(1100, 700);
        view->show();
        QVERIFY(QTest::qWaitForWindowExposed(view.get()));
        QVERIFY(view->open(id, ms, query));
    }
    QRect rowRect(int row) { return view->list()->visualRect(view->model()->index(row, 0)); }
    bool rowVisible(int row) { return view->list()->viewport()->rect().intersects(rowRect(row)); }
    QString dbEdited(qint64 id, int idx)
    {
        QSqlQuery q(db.handle());
        q.prepare("SELECT edited_text IS NULL, COALESCE(edited_text,'') FROM segments s JOIN transcripts t ON t.id=s.transcript_id"
                  " WHERE t.recording_id=? AND t.is_current=1 AND s.idx=?");
        q.addBindValue(id); q.addBindValue(idx);
        if (!q.exec() || !q.next()) return QStringLiteral("<none>");
        return q.value(0).toBool() ? QStringLiteral("<null>") : q.value(1).toString();   // SQL decides NULL vs empty string
    }

    // The event sequence a real double click produces (QTest::mouseDClick does not reach item views reliably):
    // press, release, double-click (the view needs the press on the same item), release.
    static void doubleClick(QWidget* viewport, const QPoint& pos)
    {
        QTest::mouseClick(viewport, Qt::LeftButton, {}, pos);
        QMouseEvent dbl(QEvent::MouseButtonDblClick, pos, viewport->mapToGlobal(pos), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &dbl);
        QMouseEvent rel(QEvent::MouseButtonRelease, pos, viewport->mapToGlobal(pos), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &rel);
    }

private slots:
    void init()
    {
        tok = createTokenizer();
        QVERIFY(db.open(":memory:"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("INSERT INTO folders(id,path) VALUES(1,'/f')"));
        m_n = 0;
        docker = add("meetings/docker.m4a", meetingText());
        memo = add("memos/shopping.wav", { "牛乳とトマトを買って帰ります。", "週末は家族で出かけます。" });
    }
    void cleanup() { view.reset(); }

    // ---- model
    void modelFormatsTimesAndFindsTheSegmentPlayingAtATime()
    {
        QCOMPARE(TranscriptModel::formatTime(0), QString("00:00:00"));
        QCOMPARE(TranscriptModel::formatTime(760000), QString("00:12:40"));
        QCOMPARE(TranscriptModel::formatTime(3 * 3600 * 1000 + 61000), QString("03:01:01"));
        QCOMPARE(TranscriptModel::formatTime(-5), QString("00:00:00"));
        openView(docker);
        auto* m = view->model();
        QCOMPARE(m->rowCount(), 6);
        QCOMPARE(m->rowForTime(0), 0);
        QCOMPARE(m->rowForTime(3999), 0);
        QCOMPARE(m->rowForTime(4000), 1);
        QCOMPARE(m->rowForTime(11999), 2);
        QCOMPARE(m->rowForTime(10000000), 5);                           // past the end: the last sentence
    }

    // ---- showing
    void showsHeaderKeywordsInfoAndAllSentences()
    {
        openView(docker);
        QCOMPARE(view->titleText(), RecordingDetail::load(db, docker).title.content);
        QCOMPARE(view->model()->rowCount(), 6);
        QCOMPARE(view->model()->data(view->model()->index(0, 0), Qt::DisplayRole).toString(), meetingText()[0]);
        QCOMPARE(view->model()->data(view->model()->index(2, 0), TranscriptModel::StartMsRole).toLongLong(), qint64(8000));
        QVERIFY(view->infoText().contains("ggml-small") && view->infoText().contains("ja") && view->infoText().contains("6"));
        QStringList kws;
        for (const auto& k : RecordingDetail::load(db, docker).keywords) kws << k.name;
        QCOMPARE(view->keywordTexts(), kws);
        QVERIFY(view->counterText().isEmpty());                          // no query: no counter
        QVERIFY(!view->open(9999));                                      // unknown recording
    }
    void clickingAKeywordSearchesForIt()
    {
        openView(docker);
        QPushButton* b = view->keywordButton("Podman");
        QVERIFY(b);
        QTest::mouseClick(b, Qt::LeftButton);
        QCOMPARE(view->searchEdit()->text(), QString("Podman"));
        QVERIFY(view->hitCount() >= 3);
    }

    // ---- in-page search
    void searchCountsEveryOccurrenceCaseAndWidthInsensitively()
    {
        openView(docker);
        view->setSearchText("podman");
        QCOMPARE(view->hitCount(), 4);                                   // rows 0 (Podman), 1 (Podman), 4 (ＰＯＤＭＡＮ), 5 (podman)
        QCOMPARE(view->counterText(), QString("1 / 4"));
        view->setSearchText("PODMAN");
        QCOMPARE(view->hitCount(), 4);
        view->setSearchText("ＰＯＤＭＡＮ");
        QCOMPARE(view->hitCount(), 4);
        view->setSearchText("kubernetes");
        QCOMPARE(view->hitCount(), 0);
        QCOMPARE(view->counterText(), QString("0 / 0"));
        view->setSearchText("");
        QVERIFY(view->counterText().isEmpty());
        QCOMPARE(view->currentHitIndex(), -1);
    }
    void severalTermsAreAllHighlighted()
    {
        openView(docker);
        view->setSearchText("Docker 権限");
        QVERIFY(view->hitCount() >= 3);
        const QVariantList hl = view->model()->index(2, 0).data(TranscriptModel::HighlightsRole).toList();
        QCOMPARE(hl.size(), 2);                                          // "権限" in row 2
        const QString text = view->model()->text(2);
        QCOMPARE(text.mid(hl[0].toInt(), hl[1].toInt()), QString("権限"));
        // overlapping terms merge instead of drawing twice
        view->setSearchText("権限 権限管理");
        QCOMPARE(view->model()->index(2, 0).data(TranscriptModel::HighlightsRole).toList().size(), 2);
    }
    void nextAndPreviousWrapAroundAndScrollToTheHit()
    {
        QStringList many;
        for (int i = 0; i < 300; ++i) many << (i % 50 == 0 ? QString("重要なキーワードがここに出ます(%1)。").arg(i) : QString("関係のない普通の発言その%1です。").arg(i));
        const qint64 id = add("misc/long.mp3", many);
        openView(id);
        view->setSearchText("重要なキーワード");
        QCOMPARE(view->hitCount(), 6);
        QCOMPARE(view->currentHitIndex(), 0);
        QVERIFY(rowVisible(0));
        view->nextHit();
        QCOMPARE(view->currentHitIndex(), 1);
        QCOMPARE(view->counterText(), QString("2 / 6"));
        QVERIFY2(rowVisible(50), "the second hit must be scrolled into view");
        for (int i = 0; i < 4; ++i) view->nextHit();
        QCOMPARE(view->currentHitIndex(), 5);
        QVERIFY(rowVisible(250));
        view->nextHit();                                                 // wraps to the first
        QCOMPARE(view->currentHitIndex(), 0);
        QVERIFY(rowVisible(0));
        view->previousHit();                                             // and back to the last
        QCOMPARE(view->currentHitIndex(), 5);
        // buttons and keys do the same
        QTest::mouseClick(view->nextButton(), Qt::LeftButton);
        QCOMPARE(view->currentHitIndex(), 0);
        view->searchEdit()->setFocus();
        QTest::keyClick(view->searchEdit(), Qt::Key_Return);
        QCOMPARE(view->currentHitIndex(), 1);
        QTest::keyClick(view->searchEdit(), Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(view->currentHitIndex(), 0);
    }
    void theCurrentHitIsDrawnDifferentlyFromTheOthers()
    {
        openView(docker);
        view->setSearchText("podman");
        QVERIFY(view->hitCount() >= 2);
        // What a row looks like, counted over the whole row (not one pixel at a font-dependent place): pixels of the accent
        // (blue-ish: the current hit) and of the yellow highlight (the other hits).
        struct Look { int accent = 0, yellow = 0; };
        const auto look = [&](int row) {
            const QImage img = view->list()->viewport()->grab().toImage();
            const QRect r = rowRect(row).intersected(img.rect());
            Look l;
            for (int y = r.top(); y <= r.bottom(); ++y)
                for (int x = r.left(); x <= r.right(); ++x) {
                    const QColor c = img.pixelColor(x, y);
                    if (c.blue() > c.red() + 60 && c.blue() > 120) ++l.accent;
                    else if (c.red() > 200 && c.green() > 180 && c.blue() < 170) ++l.yellow;
                }
            return l;
        };
        const int curRow = view->model()->hits()[view->currentHitIndex()].row;
        int otherRow = -1;
        for (const auto& h : view->model()->hits()) if (h.row != curRow) { otherRow = h.row; break; }
        QVERIFY(otherRow >= 0 && rowVisible(otherRow) && rowVisible(curRow));
        const Look cur = look(curRow), other = look(otherRow);
        const QString what = QStringLiteral("current: accent %1 yellow %2; other: accent %3 yellow %4").arg(cur.accent).arg(cur.yellow).arg(other.accent).arg(other.yellow);
        QVERIFY2(cur.accent > other.accent, qPrintable("the current hit should be drawn in the accent colour: " + what));
        QVERIFY2(other.yellow > cur.yellow, qPrintable("the other hits should have the yellow highlight: " + what));
    }
    void openingAtATimeWithAQueryLandsOnTheFirstHitAfterIt()
    {
        QStringList many;
        for (int i = 0; i < 200; ++i) many << (i % 40 == 5 ? QString("目印の語がある発言%1です。").arg(i) : QString("普通の発言その%1です。").arg(i));
        const qint64 id = add("misc/long2.mp3", many);                   // 4 s per sentence; hits at rows 5, 45, 85, 125, 165
        openView(id, 100 * 4000 + 500, "目印の語");                       // from 400 s on: row 100
        QCOMPARE(view->hitCount(), 5);
        QCOMPARE(view->currentHitIndex(), 3);                            // the hit in row 125
        QVERIFY(rowVisible(125));
        openView(id, 100 * 4000 + 500);                                  // without a query: just that sentence
        QVERIFY(rowVisible(100));
        QCOMPARE(view->list()->currentIndex().row(), 100);
    }

    // ---- toggles / copy
    void timeToggleHidesTheTimeColumn()
    {
        openView(docker);
        const QImage withTimes = view->list()->viewport()->grab().toImage();
        QTest::mouseClick(view->timeToggle(), Qt::LeftButton);
        QVERIFY(!view->timeToggle()->isChecked());
        const QImage without = view->list()->viewport()->grab().toImage();
        QVERIFY(withTimes != without);
        // the text moves left when times are hidden: rows are also at least as tall as before
        QTest::mouseClick(view->timeToggle(), Qt::LeftButton);
        QVERIFY(view->timeToggle()->isChecked());
    }
    void copyPutsThePlainTextOnTheClipboard()
    {
        openView(docker);
        QCOMPARE(view->copyText(false), meetingText().join('\n'));
        const QString timed = view->copyText(true);
        QVERIFY(timed.startsWith("[00:00:00] 今日は"));
        QVERIFY(timed.contains("\n[00:00:08] 共有フォルダ"));
        QClipboard* cb = QGuiApplication::clipboard();
        cb->clear();
        QMenu* menu = view->copyButton()->menu();
        QVERIFY(menu);
        menu->actions().at(1)->trigger();                                // 時刻付きでコピー
        QCOMPARE(cb->text(), timed);
        menu->actions().at(0)->trigger();
        QCOMPARE(cb->text(), meetingText().join('\n'));
    }

    // ---- seek / playback
    void clickingASentenceRequestsPlaybackFromItsStart()
    {
        openView(docker);
        QSignalSpy seek(view.get(), &TranscriptView::seekRequested);
        QTest::mouseClick(view->list()->viewport(), Qt::LeftButton, {}, rowRect(2).center());
        QCOMPARE(seek.count(), 1);
        QCOMPARE(seek.at(0).at(0).toLongLong(), qint64(8000));
        QVERIFY(!view->isEditing());
    }
    void playbackPositionHighlightsAndFollowsTheCurrentSentence()
    {
        QStringList many;
        for (int i = 0; i < 300; ++i) many << QString("これは%1番目の発言です。").arg(i);
        const qint64 id = add("misc/play.mp3", many);
        openView(id);
        QCOMPARE(view->playbackRow(), -1);
        view->setPlaybackPositionMs(150 * 4000 + 100);
        QCOMPARE(view->playbackRow(), 150);
        QVERIFY2(rowVisible(150), "follow is on: the playing sentence is scrolled into view");
        const int scroll = view->list()->verticalScrollBar()->value();
        view->setPlaybackPositionMs(150 * 4000 + 3000);                 // same sentence: no movement
        QCOMPARE(view->list()->verticalScrollBar()->value(), scroll);
        QTest::mouseClick(view->followToggle(), Qt::LeftButton);         // follow off
        QVERIFY(!view->followToggle()->isChecked());
        view->setPlaybackPositionMs(280 * 4000);
        QCOMPARE(view->playbackRow(), 280);
        QVERIFY2(!rowVisible(280), "follow is off: the view stays where the user left it");
        QCOMPARE(view->list()->verticalScrollBar()->value(), scroll);
    }

    // ---- editing
    void doubleClickingASentenceEditsItAndSavesEverything()
    {
        openView(docker);
        QSignalSpy edited(view.get(), &TranscriptView::recordingEdited);
        doubleClick(view->list()->viewport(), rowRect(3).center());
        QTRY_VERIFY(view->isEditing());                                  // starts right after the click sequence ends
        QTest::qWait(50);                                                // ... and stays open (the click's focus change must not close it)
        QVERIFY(view->isEditing());
        QVERIFY(view->editor()->isVisible());
        QCOMPARE(view->editor()->toPlainText(), meetingText()[3]);
        view->editor()->setPlainText("来週までにゼブラ担当者を決めます。");
        QTest::keyClick(view->editor(), Qt::Key_Return);
        QVERIFY(!view->isEditing());
        QCOMPARE(view->model()->text(3), QString("来週までにゼブラ担当者を決めます。"));
        QVERIFY(view->model()->isEdited(3));
        QCOMPARE(view->model()->index(3, 0).data(TranscriptModel::OriginalRole).toString(), meetingText()[3]);   // the recognized text is kept
        QCOMPARE(dbEdited(docker, 3), QString("来週までにゼブラ担当者を決めます。"));
        QVERIFY(edited.count() >= 1);
        SearchService svc(db, *tok);
        QCOMPARE(svc.search("ゼブラ").hits.size(), 1);                    // searchable at once (index rebuilt)
        QVERIFY(svc.search("検証担当者").hits.isEmpty() || svc.search("検証担当者").hits[0].segmentHits == 0);
        // the in-page search sees the corrected text too
        view->setSearchText("ゼブラ");
        QCOMPARE(view->hitCount(), 1);
    }
    void escapeCancelsAndUnchangedTextSavesNothing()
    {
        openView(docker);
        view->beginEdit(1);
        view->editor()->setPlainText("捨てる");
        QTest::keyClick(view->editor(), Qt::Key_Escape);
        QVERIFY(!view->isEditing());
        QCOMPARE(dbEdited(docker, 1), QString("<null>"));
        QCOMPARE(view->model()->text(1), meetingText()[1]);
        view->beginEdit(1);
        QTest::keyClick(view->editor(), Qt::Key_Return);                 // confirmed without changing anything
        QCOMPARE(dbEdited(docker, 1), QString("<null>"));
        QVERIFY(!view->model()->isEdited(1));
    }
    void editModeMakesASingleClickEdit()
    {
        openView(docker);
        QSignalSpy seek(view.get(), &TranscriptView::seekRequested);
        QTest::mouseClick(view->editToggle(), Qt::LeftButton);
        QVERIFY(view->editToggle()->isChecked());
        QTest::mouseClick(view->list()->viewport(), Qt::LeftButton, {}, rowRect(2).center());
        QVERIFY(view->isEditing());
        QCOMPARE(seek.count(), 0);                                       // editing, not seeking
        view->cancelEdit();
    }
    void anEditedSentenceCanBeRevertedAndBecomesRecognizedTextAgain()
    {
        openView(docker);
        view->beginEdit(2);
        view->editor()->setPlainText("手で直した文です。");
        QVERIFY(view->commitEdit());
        QVERIFY(view->model()->isEdited(2));
        QVERIFY(view->revertRow(2));
        QCOMPARE(view->model()->text(2), meetingText()[2]);
        QVERIFY(!view->model()->isEdited(2));
        QCOMPARE(dbEdited(docker, 2), QString("<null>"));
        QVERIFY(!view->revertRow(2));                                    // nothing left to revert
        // typing the recognized text back also clears the correction
        view->beginEdit(2);
        view->editor()->setPlainText("別の文");
        QVERIFY(view->commitEdit());
        view->beginEdit(2);
        view->editor()->setPlainText(meetingText()[2]);
        QVERIFY(view->commitEdit());
        QVERIFY(!view->model()->isEdited(2));
        QCOMPARE(dbEdited(docker, 2), QString("<null>"));
    }
    void deletingAllTextOfASentenceIsAValidCorrection()
    {
        openView(docker);
        view->beginEdit(4);
        view->editor()->clear();
        QVERIFY(view->commitEdit());
        QCOMPARE(dbEdited(docker, 4), QString(""));                      // empty string, not NULL
        QVERIFY(view->model()->isEdited(4));
        QVERIFY(view->model()->text(4).isEmpty());
        QVERIFY(view->revertRow(4));
        QCOMPARE(view->model()->text(4), meetingText()[4]);
    }
    void editingKeepsTheUsersTitleAndRegeneratesTheAiSummary()
    {
        QVERIFY(AnalysisRepository(db).saveUser(docker, AnalysisRepository::Title, "週次スタンドアップ"));
        const QString summaryBefore = RecordingDetail::load(db, docker).summary.content;
        openView(docker);
        view->beginEdit(0);
        view->editor()->setPlainText("今日はキリン案とゼブラ案の比較について議論します。キリン案が有力です。");
        QVERIFY(view->commitEdit());
        const RecordingDetail d = RecordingDetail::load(db, docker);
        QCOMPARE(d.title.content, QString("週次スタンドアップ"));
        QVERIFY(d.title.isUser);
        QVERIFY(!d.summary.isUser);
        QVERIFY(d.summary.content != summaryBefore || d.summary.content.contains("キリン"));
    }
    void withoutATokenizerTheViewIsReadOnly()
    {
        openView(docker, -1, QString(), false);
        QSignalSpy seek(view.get(), &TranscriptView::seekRequested);
        view->beginEdit(1);
        QVERIFY(!view->isEditing());
        QVERIFY(!view->revertRow(1));
        QTest::mouseClick(view->list()->viewport(), Qt::LeftButton, {}, rowRect(1).center());
        QCOMPARE(seek.count(), 1);                                       // navigation still works
        view->setSearchText("Podman");
        QVERIFY(view->hitCount() >= 1);
    }
    void leavingWhileEditingSavesTheText()
    {
        openView(docker);
        QSignalSpy back(view.get(), &TranscriptView::backRequested);
        view->beginEdit(1);
        view->editor()->setPlainText("戻る前に直した文です。");
        QTest::mouseClick(view->backButton(), Qt::LeftButton);
        QCOMPARE(back.count(), 1);
        QCOMPARE(dbEdited(docker, 1), QString("戻る前に直した文です。"));
    }
    void escapeClearsTheSearchThenGoesBack()
    {
        openView(docker);
        QSignalSpy back(view.get(), &TranscriptView::backRequested);
        view->setSearchText("podman");
        view->list()->setFocus();
        QTest::keyClick(view.get(), Qt::Key_Escape);
        QVERIFY(view->searchEdit()->text().isEmpty());
        QCOMPARE(back.count(), 0);
        QTest::keyClick(view.get(), Qt::Key_Escape);
        QCOMPARE(back.count(), 1);
    }
    void ctrlFFocusesTheSearchBox()
    {
        openView(docker);
        view->searchEdit()->setText("abc");
        view->list()->setFocus();
        QTest::keyClick(view.get(), Qt::Key_F, Qt::ControlModifier);
        QVERIFY(view->searchEdit()->hasFocus());
        QCOMPARE(view->searchEdit()->selectedText(), QString("abc"));
    }

    // ---- scale
    void aVeryLongTranscriptOpensQuickly()
    {
        QStringList many;
        for (int i = 0; i < 5000; ++i) many << QString("これは%1番目の発言で、少し長めの文章としてDockerとPodmanの話をしています。").arg(i);
        QSqlQuery q(db.handle());
        const qint64 id = add("misc/huge.mp3", many);
        QElapsedTimer t; t.start();
        openView(id);
        const qint64 openMs = t.elapsed();
        t.restart();
        view->setSearchText("Podman");
        const qint64 searchMs = t.elapsed();
        QCOMPARE(view->hitCount(), 5000);
        t.restart();
        view->setPlaybackPositionMs(4000LL * 4321);
        QVERIFY(rowVisible(4321));
        const qint64 followMs = t.elapsed();
        QVERIFY2(openMs < 3000 && searchMs < 1500 && followMs < 1500,
                 qPrintable(QString("open %1 ms, search %2 ms, follow %3 ms").arg(openMs).arg(searchMs).arg(followMs)));
        qInfo("5000 sentences: open %lld ms, search %lld ms, follow %lld ms", openMs, searchMs, followMs);
    }
};

class TstTranscriptWindow : public QObject {
    Q_OBJECT
    Database db;
    std::unique_ptr<ITokenizer> tok;
    std::unique_ptr<MainWindow> w;
    qint64 docker = 0;
private slots:
    void init()
    {
        tok = createTokenizer();
        QVERIFY(db.open(":memory:"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("INSERT INTO folders(id,path) VALUES(1,'/f')"));
        q.prepare("INSERT INTO recordings(folder_id,rel_path,abs_path,status,recorded_at,created_at,updated_at,duration_ms) VALUES(1,'meetings/docker.m4a','/f/docker.m4a','done',1700000000,0,0,24000)");
        QVERIFY(q.exec());
        docker = q.lastInsertId().toLongLong();
        TranscriptResult r; r.language = "ja"; r.model = "ggml-small";
        qint64 t = 0;
        for (const QString& s : QStringList{ "今日はPodmanへの移行方針について議論します。", "rootlessで動くのでメリットが大きいです。", "共有フォルダの権限管理も検討しました。",
                                              "来週までに検証担当者を決めます。", "最後にスケジュールを確認します。", "以上で終わります。" }) {
            r.segments.append({ t, t + 3800, s }); t += 4000;
        }
        QVERIFY(TranscriptRepository(db).save(docker, r));
        QVERIFY(KnowledgeBuilder(db, *tok).build(docker));
        w = std::make_unique<MainWindow>();
        w->setTokenizer(tok.get());
        w->setDatabase(&db);
        w->show();
        QVERIFY(QTest::qWaitForWindowActive(w.get()));
    }
    void cleanup() { w.reset(); }

    void startsOnTheLibraryAndOpensFromTheDetailPane()
    {
        QVERIFY(!w->isTranscriptShown());
        QVERIFY(w->findChild<QToolBar*>()->isVisible());
        w->cardView()->setCurrentIndex(w->recordingModel()->index(0, 0));
        QSignalSpy open(w.get(), &MainWindow::openTranscriptRequested);
        QTest::mouseClick(w->detailPaneWidget()->openButton(), Qt::LeftButton);
        QVERIFY(w->isTranscriptShown());
        QCOMPARE(w->transcriptView()->recordingId(), docker);
        QCOMPARE(open.count(), 1);
        QVERIFY(!w->findChild<QToolBar*>()->isVisible());                 // the full-text screen has its own header
        QTest::mouseClick(w->transcriptView()->backButton(), Qt::LeftButton);
        QVERIFY(!w->isTranscriptShown());
        QVERIFY(w->findChild<QToolBar*>()->isVisible());
    }
    void doubleClickingACardOrPressingEnterOpensIt()
    {
        auto* cards = w->cardView();
        const QModelIndex idx = w->recordingModel()->index(0, 0);
        cards->setCurrentIndex(idx);
        QTest::mouseClick(cards->viewport(), Qt::LeftButton, {}, cards->visualRect(idx).center() - QPoint(40, 10));
        QMouseEvent dbl(QEvent::MouseButtonDblClick, cards->visualRect(idx).center() - QPoint(40, 10), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(cards->viewport(), &dbl);
        QVERIFY(w->isTranscriptShown());
        w->showLibrary();
        cards->setFocus();
        cards->setCurrentIndex(idx);
        QTest::keyClick(cards, Qt::Key_Return);
        QVERIFY(w->isTranscriptShown());
        QCOMPARE(w->transcriptView()->recordingId(), docker);
    }
    void aTranscriptHitFromTheSearchOpensAtThatTimeWithTheQuery()
    {
        w->searchBox()->setFocus();
        w->searchBox()->setText("検証担当者");
        w->runSearchNow();
        auto* pop = w->searchPopup();
        QListWidgetItem* seg = nullptr;
        for (int i = 0; i < pop->count(); ++i)
            if (pop->item(i)->data(SearchSuggestPopup::KindRole).toInt() == SearchSuggestPopup::SegmentItem) seg = pop->item(i);
        QVERIFY(seg);
        pop->setCurrentItem(seg);
        QTest::keyClick(w->searchBox(), Qt::Key_Return);
        QVERIFY(w->isTranscriptShown());
        QCOMPARE(w->transcriptView()->searchEdit()->text(), QString("検証担当者"));   // the main search carries over
        QCOMPARE(w->transcriptView()->hitCount(), 1);
        QCOMPARE(w->transcriptView()->model()->hits()[0].row, 3);
        QCOMPARE(w->transcriptView()->counterText(), QString("1 / 1"));
    }
    void editsMadeInTheTranscriptAreVisibleOnTheCardsAfterGoingBack()
    {
        w->showTranscript(docker);
        auto* tv = w->transcriptView();
        tv->beginEdit(0);
        tv->editor()->setPlainText("今日はゼブラプロジェクトの移行方針について議論します。ゼブラの件が中心です。");
        QVERIFY(tv->commitEdit());
        w->showLibrary();
        // the card's summary/keywords were regenerated from the corrected text
        const QModelIndex idx = w->recordingModel()->index(w->recordingModel()->rowForId(docker), 0);
        const QString summary = idx.data(RecordingListModel::SummaryRole).toString();
        const QStringList kws = idx.data(RecordingListModel::KeywordsRole).toStringList();
        QVERIFY2(summary.contains("ゼブラ") || kws.join(",").contains("ゼブラ"), qPrintable(summary + " | " + kws.join(",")));
    }
    void aClickInTheTranscriptAsksTheWindowToPlayFromThere()
    {
        w->showTranscript(docker);
        QSignalSpy seek(w.get(), &MainWindow::seekRequested);
        auto* list = w->transcriptView()->list();
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualRect(w->transcriptView()->model()->index(3, 0)).center());
        QCOMPARE(seek.count(), 1);
        QCOMPARE(seek.at(0).at(0).toLongLong(), docker);
        QCOMPARE(seek.at(0).at(1).toLongLong(), qint64(12000));
    }
    void closingTheWindowWhileEditingIsSafe()
    {
        w->showTranscript(docker);
        w->transcriptView()->beginEdit(1);
        QVERIFY(w->transcriptView()->isEditing());
        w.reset();                                                          // must not crash (ASan checks this too)
    }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    int rc = 0;
    { TstTranscriptUi t; rc |= QTest::qExec(&t, argc, argv); }
    { TstTranscriptWindow t; rc |= QTest::qExec(&t, argc, argv); }
    return rc;
}
#include "tst_transcript_ui.moc"
