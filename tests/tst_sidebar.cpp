#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QStatusBar>
#include <QtTest>

#include "core/Database.h"
#include "core/DemoData.h"
#include "ui/CardListView.h"
#include "ui/MainWindow.h"
#include "ui/RecordingListModel.h"
#include "ui/SidebarWidget.h"
#include "ui/TagPickerDialog.h"

using namespace ake;
using Scope = RecordingFilter::Scope;

class TstSidebar : public QObject {
    Q_OBJECT
    Database db;
    std::unique_ptr<MainWindow> w;

    SidebarWidget* sb() { return w->sidebarWidget(); }
    int rows() { return w->recordingModel()->rowCount(); }
    QString headerTitle() { return w->findChild<QLabel*>("cardTitle")->text(); }
    QString headerCount() { return w->findChild<QLabel*>("cardCount")->text(); }
    QTreeWidgetItem* item(SidebarWidget::Kind k, qint64 key) { return sb()->findItem(k, key); }
    QTreeWidgetItem* tagItem(const QString& name)
    {
        for (const auto& t : sb()->data().tags)
            if (t.name == name) return item(SidebarWidget::Tag, t.id);
        return nullptr;
    }
    void click(QTreeWidgetItem* it)
    {
        QVERIFY(it);
        const QRect r = sb()->visualItemRect(it);
        QTest::mouseClick(sb()->viewport(), Qt::LeftButton, {}, r.center());
    }

private slots:
    void init()
    {
        QVERIFY(db.open(":memory:"));
        QVERIFY(seedDemoData(db));
        w = std::make_unique<MainWindow>();
        w->setDatabase(&db);
        w->show();
        QVERIFY(QTest::qWaitForWindowExposed(w.get()));
    }
    void cleanup() { w.reset(); }

    void showsSectionsAndCounts()
    {
        auto* all = item(SidebarWidget::Library, int(Scope::All));
        QCOMPARE(all->data(0, SidebarWidget::CountRole).toInt(), 9);
        QCOMPARE(item(SidebarWidget::Library, int(Scope::Favorites))->data(0, SidebarWidget::CountRole).toInt(), 1);
        QCOMPARE(item(SidebarWidget::Library, int(Scope::Error))->data(0, SidebarWidget::CountRole).toInt(), 1);
        QCOMPARE(item(SidebarWidget::Library, int(Scope::Unanalyzed))->data(0, SidebarWidget::CountRole).toInt(), 2);
        QVERIFY(all->data(0, SidebarWidget::ActiveRole).toBool());
        // folder shows its base name; full path in tooltip
        auto* folder = item(SidebarWidget::Folder, 1);
        QCOMPARE(folder->text(0), QString("recordings"));
        QCOMPARE(folder->toolTip(0), QString("/demo/recordings"));
        // only the top tags are listed, plus the "all tags" entry
        int tags = 0;
        for (int i = 0; i < sb()->topLevelItemCount(); ++i)
            tags += sb()->topLevelItem(i)->data(0, SidebarWidget::KindRole).toInt() == SidebarWidget::Tag;
        QCOMPARE(tags, SidebarWidget::kTopTags);
        QVERIFY(item(SidebarWidget::MoreTags, 0));
        QCOMPARE(rows(), 9);
    }
    void clickingLibraryItemsFiltersCards()
    {
        QSignalSpy spy(sb(), &SidebarWidget::filterChanged);
        click(item(SidebarWidget::Library, int(Scope::Favorites)));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(rows(), 1);
        QCOMPARE(headerTitle(), QString("お気に入り"));
        QVERIFY(item(SidebarWidget::Library, int(Scope::Favorites))->data(0, SidebarWidget::ActiveRole).toBool());
        QVERIFY(!item(SidebarWidget::Library, int(Scope::All))->data(0, SidebarWidget::ActiveRole).toBool());
        click(item(SidebarWidget::Library, int(Scope::Error)));
        QCOMPARE(rows(), 1);
        QCOMPARE(w->recordingModel()->index(0, 0).data(RecordingListModel::TitleRole).toString(), QString("broken_rec.flac"));
        click(item(SidebarWidget::Library, int(Scope::All)));
        QCOMPARE(rows(), 9);
        QCOMPARE(headerCount(), QString("9件"));
    }
    void folderSelectionFilters()
    {
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("INSERT INTO folders(id,path) VALUES(2,'/home/u/podcasts')"));
        QVERIFY(q.exec("INSERT INTO recordings(folder_id,rel_path,abs_path,created_at,updated_at) VALUES(2,'z.mp3','/home/u/podcasts/z.mp3',0,0)"));
        w->refreshCards();
        QCOMPARE(rows(), 10);
        click(item(SidebarWidget::Folder, 2));
        QCOMPARE(rows(), 1);
        QCOMPARE(headerTitle(), QString("podcasts"));
        click(item(SidebarWidget::Folder, 1));
        QCOMPARE(rows(), 9);
        // switching to a library item leaves folder scope
        click(item(SidebarWidget::Library, int(Scope::All)));
        QCOMPARE(rows(), 10);
    }
    void tagsToggleAndCombineWithAnd()
    {
        click(tagItem("podman"));
        QCOMPARE(rows(), 2);
        QCOMPARE(headerCount(), QString("2件 ・ #podman"));
        click(tagItem("docker"));
        QCOMPARE(rows(), 1);
        QCOMPARE(headerCount(), QString("1件 ・ #podman ＋ #docker"));
        click(tagItem("podman")); // toggle off
        QCOMPARE(rows(), 1);
        click(tagItem("docker"));
        QCOMPARE(rows(), 9);
        QVERIFY(sb()->filter().isDefault());
    }
    void periodTogglesOnSecondClick()
    {
        auto* y2026 = item(SidebarWidget::PeriodItem, 2026);
        QVERIFY(y2026);
        click(y2026);
        QCOMPARE(sb()->filter().period, RecordingFilter::Period::Year);
        QCOMPARE(rows(), 9);
        QCOMPARE(headerCount(), QString("9件 ・ 2026年"));
        click(item(SidebarWidget::PeriodItem, 2026));
        QCOMPARE(sb()->filter().period, RecordingFilter::Period::None);
        // this month (real clock): demo data is old, so nothing matches -- and the combination with a scope works
        click(item(SidebarWidget::PeriodItem, int(RecordingFilter::Period::ThisMonth)));
        click(item(SidebarWidget::Library, int(Scope::Favorites)));
        QCOMPARE(sb()->filter().scope, Scope::Favorites);
        QCOMPARE(sb()->filter().period, RecordingFilter::Period::ThisMonth);
    }
    void keyboardEnterActivates()
    {
        auto* fav = item(SidebarWidget::Library, int(Scope::Favorites));
        sb()->setFocus();
        sb()->setCurrentItem(fav);
        QTest::keyClick(sb(), Qt::Key_Return);
        QCOMPARE(rows(), 1);
    }
    void headersAreNotActivatable()
    {
        QSignalSpy spy(sb(), &SidebarWidget::filterChanged);
        click(sb()->topLevelItem(0)); // "ライブラリ" header
        QCOMPARE(spy.count(), 0);
        QCOMPARE(rows(), 9);
    }
    void activeRareTagStaysVisible()
    {
        const auto& tags = sb()->data().tags;
        QVERIFY(tags.size() > SidebarWidget::kTopTags);
        const auto rare = tags.last();
        QVERIFY(!item(SidebarWidget::Tag, rare.id));
        RecordingFilter f; f.tagIds = { rare.id };
        sb()->setFilter(f);
        QVERIFY(item(SidebarWidget::Tag, rare.id));
        QVERIFY(item(SidebarWidget::Tag, rare.id)->data(0, SidebarWidget::ActiveRole).toBool());
        QCOMPARE(rows(), 1);
    }
    void moreTagsRequestsDialogAndPickerReturnsSelection()
    {
        QSignalSpy spy(sb(), &SidebarWidget::allTagsRequested);
        // click the entry without letting the modal dialog block: the slot is connected by MainWindow, so
        // check the signal via the widget API instead of exec()
        QTimer::singleShot(0, [] {  // close the modal dialog MainWindow opens
            for (QWidget* top : QApplication::topLevelWidgets())
                if (auto* d = qobject_cast<QDialog*>(top)) d->reject();
        });
        click(item(SidebarWidget::MoreTags, 0));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(rows(), 9); // cancelled: unchanged

        const auto& tags = sb()->data().tags;
        TagPickerDialog dlg(tags, { tags[0].id }, nullptr);
        QCOMPARE(dlg.list()->count(), tags.size());
        QCOMPARE(dlg.selectedTagIds(), QList<qint64>({ tags[0].id }));
        dlg.list()->item(2)->setCheckState(Qt::Checked);
        QCOMPARE(dlg.selectedTagIds().size(), 2);
        dlg.filterEdit()->setText("pod");
        int visible = 0;
        for (int i = 0; i < dlg.list()->count(); ++i) visible += !dlg.list()->item(i)->isHidden();
        QVERIFY(visible >= 1 && visible < tags.size());
        QCOMPARE(dlg.selectedTagIds().size(), 2); // hidden rows keep their state
    }
    void acceptingTagDialogAppliesSelection()
    {
        const auto tags = sb()->data().tags;
        const qint64 rareId = tags.last().id; // not in the sidebar's top list
        QTimer::singleShot(0, [&] {
            for (QWidget* top : QApplication::topLevelWidgets())
                if (auto* d = qobject_cast<TagPickerDialog*>(top)) {
                    for (int i = 0; i < d->list()->count(); ++i)
                        if (d->list()->item(i)->data(Qt::UserRole).toLongLong() == rareId)
                            d->list()->item(i)->setCheckState(Qt::Checked);
                    d->accept();
                }
        });
        click(item(SidebarWidget::MoreTags, 0));
        QCOMPARE(sb()->filter().tagIds, QList<qint64>({ rareId }));
        QCOMPARE(rows(), 1);
        QVERIFY(item(SidebarWidget::Tag, rareId)->data(0, SidebarWidget::ActiveRole).toBool());
    }
    void vanishedSelectionFallsBackToAll()
    {
        RecordingFilter f; f.scope = Scope::Folder; f.folderId = 99;
        sb()->setFilter(f);
        QCOMPARE(rows(), 0);
        QSignalSpy spy(sb(), &SidebarWidget::filterChanged);
        w->refreshCards();   // folder 99 does not exist -> dropped
        QCOMPARE(spy.count(), 1);
        QCOMPARE(sb()->filter().scope, Scope::All);
        QCOMPARE(rows(), 9);
    }
    void filterSurvivesRefreshAndCountsUpdate()
    {
        click(tagItem("podman"));
        QSqlQuery q(db.handle());
        QVERIFY(q.exec("UPDATE recordings SET favorite=1"));
        w->refreshCards();
        QCOMPARE(sb()->filter().tagIds.size(), 1);
        QCOMPARE(rows(), 2);
        QCOMPARE(item(SidebarWidget::Library, int(Scope::Favorites))->data(0, SidebarWidget::CountRole).toInt(), 9);
    }
};

QTEST_MAIN(TstSidebar)
#include "tst_sidebar.moc"
