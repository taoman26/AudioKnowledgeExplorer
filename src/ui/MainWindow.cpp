#include "ui/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QActionGroup>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QSqlQuery>
#include <QKeyEvent>
#include <QMimeData>
#include <QMenu>
#include <QMenuBar>
#include <QDateTime>
#include <QHBoxLayout>
#include <QShortcut>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include "core/Database.h"
#include "core/IAudioPlayer.h"
#include "core/Library.h"
#include "core/ScanService.h"
#include "core/Settings.h"
#include "core/ITokenizer.h"
#include "core/JobScheduler.h"
#include "core/SearchService.h"
#include "ui/CardListView.h"
#include "ui/DetailPane.h"
#include "ui/FilterChipBar.h"
#include "ui/FolderDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/MiniPlayer.h"
#include "ui/PlaybackController.h"
#include "ui/QueueStatusWidget.h"
#include "ui/SearchSuggestPopup.h"
#include "ui/RecordingListModel.h"
#include "ui/SidebarWidget.h"
#include "ui/TagPickerDialog.h"
#include "ui/TranscriptView.h"
#include "ui/Theme.h"

namespace ake {

static QWidget* makePane(const QString& objectName, const QString& label)
{
    auto* w = new QWidget;
    w->setObjectName(objectName);
    w->setAttribute(Qt::WA_StyledBackground, true);
    auto* l = new QVBoxLayout(w);
    auto* t = new QLabel(label);
    t->setObjectName("placeholder");
    t->setAlignment(Qt::AlignCenter);
    l->addWidget(t);
    return w;
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("AudioKnowledgeExplorer");
    resize(1480, 860);
    static const QPalette systemPalette = QApplication::palette();   // before any theme replaces it: what "follow the system" means
    m_systemPalette = systemPalette;
    applyTheme(Theme::forPalette(m_systemPalette));

    buildMenus();
    buildToolBar();
    buildPanes();
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(kSearchDelayMs);
    connect(m_searchTimer, &QTimer::timeout, this, [this] { runSearchNow(); });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& t) { onSearchTextChanged(t); });
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        if (m_popup && m_popup->isVisible() && m_popup->activateCurrent()) return;   // a highlighted suggestion wins
        runSearchNow();
        hideSuggestions();
    });
    m_search->installEventFilter(this);
    auto* focusSearch = new QShortcut(QKeySequence("Ctrl+K"), this);
    connect(focusSearch, &QShortcut::activated, this, [this] { m_search->setFocus(); m_search->selectAll(); });

    m_popup = new SearchSuggestPopup(this);
    m_popup->setTheme(m_theme);
    connect(m_popup, &SearchSuggestPopup::tagChosen, this, [this](qint64 id, const QString&) {
        RecordingFilter f = m_sidebarWidget->filter();
        if (!f.tagIds.contains(id)) f.tagIds << id;
        clearSearchUi();                       // the tag replaces what was typed
        m_sidebarWidget->setFilter(f);
    });
    connect(m_popup, &SearchSuggestPopup::recordingChosen, this, [this](qint64 id) { selectRecording(id); });
    connect(m_popup, &SearchSuggestPopup::segmentChosen, this, [this](qint64 id, qint64 ms) {
        const QString query = m_search->text().trimmed();   // selectRecording() clears the box; the transcript gets it as its search
        selectRecording(id);
        showTranscript(id, ms, query);
    });

    // What the user picks is remembered for the next start (setViewMode / setSortOrder called by code are not).
    connect(m_viewCards, &QAction::triggered, this, [this] {
        m_cards->setMode(CardListView::Mode::Cards);
        if (m_db) Settings(*m_db).set(Settings::kViewMode, "cards");
    });
    connect(m_viewRows, &QAction::triggered, this, [this] {
        m_cards->setMode(CardListView::Mode::Rows);
        if (m_db) Settings(*m_db).set(Settings::kViewMode, "rows");
    });
    for (QAction* act : m_sortActions)
        connect(act, &QAction::triggered, this, [this, act] {
            const auto order = RecordingListModel::SortOrder(act->data().toInt());
            setSortOrder(order);
            if (m_db) Settings(*m_db).set(Settings::kSortOrder, order == RecordingListModel::SortOrder::Oldest ? "oldest"
                                                              : order == RecordingListModel::SortOrder::Longest ? "longest" : "newest");
        });

    m_scanLabel = new QLabel;
    m_scanLabel->setObjectName("detailSub");
    m_scanLabel->hide();
    statusBar()->addPermanentWidget(m_scanLabel);
    setAcceptDrops(true);                                   // drop a folder anywhere to register it
    connect(m_addFolder, &QAction::triggered, this, [this] {
        const QString dir = m_chooser ? m_chooser() : QFileDialog::getExistingDirectory(this, tr("録音フォルダを選択"), QString(), QFileDialog::ShowDirsOnly);
        if (!dir.isEmpty()) addFolder(dir);
    });
    connect(m_folderDialog, &QAction::triggered, this, [this] { showFolderDialog(); });
    connect(m_settingsAction, &QAction::triggered, this, [this] { showSettingsDialog(); });

    m_queueStatus = new QueueStatusWidget;
    statusBar()->addPermanentWidget(m_queueStatus);
    statusBar()->showMessage(tr("0件"));
}

MainWindow::~MainWindow()
{
    if (m_playback) m_playback->stop();               // no audio after the window is gone
    if (m_detailPane) m_detailPane->aboutToClose();
    if (m_transcript) m_transcript->aboutToClose();   // before QWidget's destructor closes the window and moves the focus
    delete m_searchSvc;
}

void MainWindow::applyTheme(const Theme& t)
{
    m_theme = t;
    if (m_cards) {
        m_cards->setTheme(t);
    }
    if (m_sidebarWidget) m_sidebarWidget->setTheme(t);
    if (m_popup) m_popup->setTheme(t);
    if (m_detailPane) m_detailPane->setTheme(t);
    if (m_transcript) m_transcript->setTheme(t);
    QPalette p = m_systemPalette;               // always start from the system palette, so "system" really restores it
    p.setColor(QPalette::Window, t.bg);
    p.setColor(QPalette::Base, t.panel);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::PlaceholderText, t.faint);
    p.setColor(QPalette::Highlight, t.accent);
    p.setColor(QPalette::Button, t.panel);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::AlternateBase, t.side);
    p.setColor(QPalette::ToolTipBase, t.panel);
    p.setColor(QPalette::ToolTipText, t.text);
    QApplication::setPalette(p);                 // application-wide: dialogs are windows of their own and would stay light
    qApp->setStyleSheet(t.styleSheet());
    // Children created earlier keep their old polish; force a re-evaluation of the stylesheet.
    for (QWidget* w : findChildren<QWidget*>()) {
        w->style()->unpolish(w);
        w->style()->polish(w);
    }
}

void MainWindow::applyThemeName(const QString& name)
{
    if (name == "dark") applyTheme(Theme::dark());
    else if (name == "light") applyTheme(Theme::light());
    else applyTheme(Theme::forPalette(m_systemPalette));
}

SettingsDialog* MainWindow::createSettingsDialog()
{
    if (!m_db) return nullptr;
    Settings st(*m_db);
    auto* dlg = new SettingsDialog(st, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QDialog::accepted, this, [this] { applySettings(); });
    return dlg;
}

void MainWindow::showSettingsDialog()
{
    if (SettingsDialog* dlg = createSettingsDialog()) dlg->exec();
}

void MainWindow::applySettings()
{
    if (!m_db) return;
    const Settings st(*m_db);
    if (m_scheduler) m_scheduler->setConfig(st.analysisConfig());   // read when the next job starts
    applyThemeName(st.theme());
    updateModelBanner();
    emit settingsChanged();
}

bool MainWindow::modelMissing() const
{
    if (!m_scheduler) return false;
    const AnalysisConfig& c = m_scheduler->config();
    return !QFileInfo::exists(c.modelPath.isEmpty() ? AnalysisConfig::defaultModelPath() : c.modelPath);
}

void MainWindow::updateModelBanner()
{
    if (!m_modelBanner) return;
    bool show = false;
    if (m_db && modelMissing()) {                 // with no recording yet the empty state leads the way
        QSqlQuery q(m_db->handle());
        show = q.exec("SELECT 1 FROM recordings LIMIT 1") && q.next();
    }
    if (show) {
        const AnalysisConfig& c = m_scheduler->config();
        m_modelBannerText->setText(tr("文字起こしのモデルが見つからないため、録音を解析できません。\n%1 にモデルファイル（ggml）を置くか、設定で別のモデルを選んでください。")
                                       .arg(c.modelPath.isEmpty() ? AnalysisConfig::defaultModelPath() : c.modelPath));
    }
    m_modelBanner->setVisible(show);
}

void MainWindow::buildMenus()
{
    for (const char* m : { "ファイル", "編集", "表示", "ライブラリ", "ツール", "ヘルプ" }) {
        QMenu* menu = menuBar()->addMenu(QString::fromUtf8(m));
        if (QString::fromUtf8(m) == "ファイル") {
            m_addFolder = menu->addAction(tr("＋ フォルダ追加"));
            m_addFolder->setShortcut(QKeySequence("Ctrl+O"));
            m_folderDialog = menu->addAction(tr("フォルダ管理…"));
            m_folderDialog->setShortcut(QKeySequence("Ctrl+Shift+O"));
            menu->addSeparator();
            QAction* quit = menu->addAction(tr("終了"));
            quit->setShortcut(QKeySequence::Quit);
            connect(quit, &QAction::triggered, this, &QWidget::close);
            continue;
        }
        if (QString::fromUtf8(m) == "ツール") {
            m_settingsAction = menu->addAction(tr("設定…"));
            m_settingsAction->setShortcut(QKeySequence("Ctrl+,"));
            continue;
        }
        if (QString::fromUtf8(m) == "表示") {
            auto* views = new QActionGroup(menu);
            m_viewCards = menu->addAction(tr("カード表示"));
            m_viewRows = menu->addAction(tr("リスト表示"));
            for (QAction* a : { m_viewCards, m_viewRows }) { a->setCheckable(true); views->addAction(a); }
            m_viewCards->setChecked(true);
            m_viewCards->setShortcut(QKeySequence("Ctrl+1"));
            m_viewRows->setShortcut(QKeySequence("Ctrl+2"));
            menu->addSeparator();
            auto* sorts = new QActionGroup(menu);
            const std::pair<const char*, RecordingListModel::SortOrder> orders[] = {
                { "新しい順", RecordingListModel::SortOrder::Newest }, { "古い順", RecordingListModel::SortOrder::Oldest },
                { "長い順", RecordingListModel::SortOrder::Longest } };
            for (const auto& o : orders) {
                QAction* a = menu->addAction(QString::fromUtf8(o.first));
                a->setCheckable(true);
                a->setData(int(o.second));
                a->setChecked(o.second == m_sortOrder);
                sorts->addAction(a);
                m_sortActions << a;
            }
            continue;
        }
        if (QString::fromUtf8(m) != "ライブラリ") continue;
        m_analyzeAll = menu->addAction(tr("未解析をすべて解析"));
        m_pause = menu->addAction(tr("解析を一時停止"));
        m_pause->setCheckable(true);
        m_cancelAll = menu->addAction(tr("解析をすべてキャンセル"));
        menu->addSeparator();
        m_rescanAll = menu->addAction(tr("フォルダを今すぐ確認"));
        m_rescanAll->setShortcut(QKeySequence("F5"));
    }
}

void MainWindow::buildToolBar()
{
    auto* tb = addToolBar(tr("ツールバー"));
    m_toolbar = tb;
    tb->setMovable(false);
    tb->addAction(m_addFolder);              // "＋ フォルダ追加" (same action as the 追加 entry of the ファイル menu)
    tb->addAction(m_folderDialog);           // "フォルダ管理…": remove, rescan, exclude patterns
    m_search = new QLineEdit;
    m_search->setPlaceholderText(tr("録音の内容を検索（タイトル・要約・タグ・全文）   Ctrl+K"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedWidth(560);
    tb->addWidget(m_search);

    auto* spacer = new QWidget;                      // pushes the view controls to the right edge
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);
    m_sortBtn = new QToolButton;
    m_sortBtn->setObjectName("sortButton");
    m_sortBtn->setPopupMode(QToolButton::InstantPopup);
    m_sortBtn->setToolTip(tr("並び順"));
    auto* sortMenu = new QMenu(m_sortBtn);
    for (QAction* a : m_sortActions) sortMenu->addAction(a);
    m_sortBtn->setMenu(sortMenu);
    m_sortBtn->setText(tr("新しい順 ▾"));
    tb->addWidget(m_sortBtn);
    // The buttons are separate widgets (a default action would overwrite their ▦ / ☰ text); state is kept in sync below.
    m_viewCardsBtn = new QToolButton;
    m_viewRowsBtn = new QToolButton;
    for (QToolButton* b : { m_viewCardsBtn, m_viewRowsBtn }) { b->setObjectName("viewToggle"); b->setCheckable(true); }
    m_viewCardsBtn->setText("▦");
    m_viewRowsBtn->setText("☰");
    m_viewCardsBtn->setToolTip(tr("カード表示 (Ctrl+1)"));
    m_viewRowsBtn->setToolTip(tr("リスト表示 (Ctrl+2)"));
    m_viewCardsBtn->setChecked(true);
    auto link = [](QToolButton* b, QAction* a) {
        QObject::connect(b, &QToolButton::clicked, a, [b, a] { b->setChecked(true); a->trigger(); if (!a->isChecked()) a->setChecked(true); });
        QObject::connect(a, &QAction::toggled, b, [b](bool on) { QSignalBlocker block(b); b->setChecked(on); });
    };
    link(m_viewCardsBtn, m_viewCards);
    link(m_viewRowsBtn, m_viewRows);
    tb->addWidget(m_viewCardsBtn);
    tb->addWidget(m_viewRowsBtn);
}

void MainWindow::setViewMode(CardListView::Mode mode)
{
    (mode == CardListView::Mode::Rows ? m_viewRows : m_viewCards)->setChecked(true);
    m_cards->setMode(mode);
}

void MainWindow::setSortOrder(RecordingListModel::SortOrder order)
{
    m_sortOrder = order;
    for (QAction* a : m_sortActions) {
        const bool on = a->data().toInt() == int(order);
        a->setChecked(on);
        if (on) m_sortBtn->setText(a->text() + " ▾");
    }
    if (m_model) m_model->setSortOrder(order);
}

void MainWindow::buildCardArea()
{
    auto* v = new QVBoxLayout(m_cardArea);
    v->setContentsMargins(20, 14, 6, 0);
    v->setSpacing(8);
    auto* head = new QHBoxLayout;
    m_cardTitle = new QLabel(tr("すべて"));
    m_cardTitle->setObjectName("cardTitle");
    m_cardCount = new QLabel;
    m_cardCount->setObjectName("cardCount");
    head->addWidget(m_cardTitle);
    head->addWidget(m_cardCount);
    head->addStretch();
    v->addLayout(head);
    m_chips = new FilterChipBar;
    v->addWidget(m_chips);
    m_modelBanner = new QWidget;
    m_modelBanner->setObjectName("modelBanner");
    auto* bh = new QHBoxLayout(m_modelBanner);
    bh->setContentsMargins(12, 8, 12, 8);
    m_modelBannerText = new QLabel;
    m_modelBannerText->setWordWrap(true);
    auto* bannerBtn = new QPushButton(tr("設定を開く"));
    bannerBtn->setCursor(Qt::PointingHandCursor);
    bh->addWidget(m_modelBannerText, 1);
    bh->addWidget(bannerBtn);
    m_modelBanner->hide();
    v->addWidget(m_modelBanner);
    connect(bannerBtn, &QPushButton::clicked, this, [this] { showSettingsDialog(); });
    connect(m_chips, &FilterChipBar::removeRequested, this, [this](int kind, qint64 key) { removeCondition(kind, key); });
    connect(m_chips, &FilterChipBar::clearAllRequested, this, [this] { clearConditions(); });
    m_facetTimer = new QTimer(this);                 // the sidebar is rebuilt after the click that changed the filter is over
    m_facetTimer->setSingleShot(true);
    connect(m_facetTimer, &QTimer::timeout, this, [this] { refreshSidebar(); updateHeader(); });   // chips and messages name tags from the new data
    m_cards = new CardListView;
    m_cards->setTheme(m_theme);
    v->addWidget(m_cards, 1);

    m_empty = new QWidget;
    m_empty->setObjectName("emptyState");
    auto* ev = new QVBoxLayout(m_empty);
    ev->setAlignment(Qt::AlignCenter);
    ev->setSpacing(10);
    auto* icon = new QLabel(QStringLiteral("♪"));
    icon->setObjectName("emptyIcon");
    icon->setAlignment(Qt::AlignCenter);
    m_emptyTitle = new QLabel;
    m_emptyTitle->setObjectName("emptyTitle");
    m_emptyTitle->setAlignment(Qt::AlignCenter);
    m_emptyText = new QLabel;
    m_emptyText->setObjectName("emptyText");
    m_emptyText->setAlignment(Qt::AlignCenter);
    m_emptyText->setWordWrap(true);
    m_emptyText->setMinimumWidth(400);               // a narrow label breaks Japanese in the middle of a word
    m_emptyText->setMaximumWidth(520);
    m_emptyAdd = new QPushButton(tr("＋ フォルダを追加"));
    m_emptyAdd->setObjectName("primary");
    m_emptyAdd->setCursor(Qt::PointingHandCursor);
    ev->addStretch();
    ev->addWidget(icon);
    ev->addWidget(m_emptyTitle);
    ev->addWidget(m_emptyText);
    ev->addSpacing(6);
    ev->addWidget(m_emptyAdd, 0, Qt::AlignCenter);
    m_emptyClear = new QPushButton(tr("絞り込みを外す"));
    m_emptyClear->setObjectName("primary");
    m_emptyClear->setCursor(Qt::PointingHandCursor);
    ev->addWidget(m_emptyClear, 0, Qt::AlignCenter);
    m_emptyClear->hide();
    ev->addStretch();
    m_empty->hide();
    v->addWidget(m_empty, 1);
    connect(m_emptyAdd, &QPushButton::clicked, this, [this] { if (m_addFolder) m_addFolder->trigger(); });
    connect(m_emptyClear, &QPushButton::clicked, this, [this] { m_sidebarWidget->setFilter(RecordingFilter()); });   // the search words stay
    connect(m_cards, &QAbstractItemView::activated, this, [this](const QModelIndex& idx) {
        if (idx.data(RecordingListModel::StateRole).value<CardState>() == CardState::Done)   // only analysed recordings have a text
            showTranscript(idx.data(RecordingListModel::IdRole).toLongLong());
    });
    connect(m_cards, &CardListView::cardActionRequested, this, [this](qint64 id, CardAction a) { if (a == CardAction::Play) togglePlayback(id); });
    m_cards->installEventFilter(this);                      // Space = play / pause
    connect(m_cards, &CardListView::currentRecordingChanged, this, [this](qint64 id) { if (m_detailPane) m_detailPane->showRecording(id); });
}

void MainWindow::setDatabase(Database* db)
{
    m_db = db;
    delete m_model;
    m_model = db ? new RecordingListModel(*db, this) : nullptr;
    if (m_db) {                                   // the last choices of the user
        const Settings st(*m_db);                 // only what the user stored: a choice made in code beforehand stays
        if (st.contains(Settings::kViewMode))
            setViewMode(st.viewMode() == "rows" ? CardListView::Mode::Rows : CardListView::Mode::Cards);
        if (st.contains(Settings::kSortOrder))
            setSortOrder(st.sortOrder() == "oldest" ? RecordingListModel::SortOrder::Oldest
                         : st.sortOrder() == "longest" ? RecordingListModel::SortOrder::Longest : RecordingListModel::SortOrder::Newest);
    }
    if (m_model) m_model->setSortOrder(m_sortOrder);
    m_cards->setModel(m_model);
    if (m_tokenizer) setTokenizer(m_tokenizer);   // the search service belongs to the database
    updateModelBanner();
    m_detailPane->setDatabase(db, m_tokenizer);
    m_transcript->setDatabase(db, m_tokenizer);
    refreshCards();
}

void MainWindow::refreshCards()
{
    refreshSidebar();
    if (m_model) {
        const qint64 keep = m_cards->currentIndex().data(RecordingListModel::IdRole).toLongLong();
        m_model->setFilter(m_sidebarWidget->filter());
        m_model->reload();
        if (keep) {                                          // a reload resets the model: put the selection back
            const int row = m_model->rowForId(keep);
            if (row >= 0) m_cards->setCurrentIndex(m_model->index(row, 0));
        }
    }
    updateHeader();
}

QString MainWindow::titleOf(qint64 id) const
{
    if (!m_model) return QString::number(id);
    const int row = m_model->rowForId(id);
    return row < 0 ? QString::number(id) : m_model->index(row, 0).data(RecordingListModel::TitleRole).toString();
}

void MainWindow::setScheduler(JobScheduler* scheduler)
{
    m_scheduler = scheduler;
    if (!scheduler) return;
    connect(scheduler, &JobScheduler::recordingChanged, this, [this](qint64 id) { onRecordingChanged(id); });
    connect(scheduler, &JobScheduler::progressChanged, this,
            [this](qint64 id, const QString& stage, double v) { onProgress(id, stage, v); });
    connect(scheduler, &JobScheduler::queueChanged, this, [this] { updateQueueStatus(); });
    connect(m_cards, &CardListView::cardActionRequested, this, [this](qint64 id, CardAction a) {
        if (a == CardAction::Analyze || a == CardAction::Retry) m_scheduler->enqueue(id);
    });
    updateModelBanner();
    connect(m_analyzeAll, &QAction::triggered, this, [this] { m_scheduler->enqueueAllNew(); });
    connect(m_pause, &QAction::toggled, this, [this](bool on) { m_scheduler->setPaused(on); });
    connect(m_cancelAll, &QAction::triggered, this, [this] { m_scheduler->cancelAll(); });
    m_detailPane->setReanalyzeEnabled(true);
    updateQueueStatus();
}

void MainWindow::onRecordingChanged(qint64 id)
{
    if (!m_model || !m_db) return;
    m_model->refreshRecording(id);
    refreshSidebar();
    updateHeader();
    if (m_detailPane && m_detailPane->currentId() == id) m_detailPane->reload();
}

void MainWindow::onProgress(qint64 id, const QString& stage, double overall)
{
    if (m_model) m_model->setLiveProgress(id, stage, overall);
    const QString what = stage == "decode" ? tr("デコード中") : stage == "load" ? tr("モデル読み込み中") : tr("文字起こし中");
    m_queueStatus->setRunning(titleOf(id), what, overall);
}

void MainWindow::updateQueueStatus()
{
    if (!m_scheduler) return;
    if (m_scheduler->runningId() == 0) m_queueStatus->setIdle();
    m_queueStatus->setQueued(m_scheduler->queuedCount(), m_scheduler->isPaused());
}

void MainWindow::setTokenizer(const ITokenizer* tokenizer)
{
    m_tokenizer = tokenizer;
    delete m_searchSvc;
    m_searchSvc = (tokenizer && m_db) ? new SearchService(*m_db, *tokenizer) : nullptr;
    if (m_detailPane) m_detailPane->setDatabase(m_db, tokenizer);
    if (m_transcript) m_transcript->setDatabase(m_db, tokenizer);
}

void MainWindow::onSearchTextChanged(const QString& text)
{
    if (text.trimmed().isEmpty()) {            // cleared: back to the normal list
        m_searchTimer->stop();
        hideSuggestions();
        if (m_model && m_model->isSearching()) { m_model->clearSearch(); refreshSidebar(); updateHeader(); }
        return;
    }
    m_searchTimer->start();                    // debounce: search once typing pauses
}

void MainWindow::runSearchNow()
{
    m_searchTimer->stop();
    const QString text = m_search->text();
    if (!m_model || !m_searchSvc || text.trimmed().isEmpty()) return;
    m_model->setSearchResult(m_searchSvc->search(text));
    refreshSidebar();                            // the sidebar now counts inside the result
    updateHeader();
    if (m_search->hasFocus()) showSuggestions();
}

void MainWindow::showSuggestions()
{
    if (!m_searchSvc || !m_popup) return;
    const Suggestions s = m_searchSvc->suggest(m_search->text());
    if (s.isEmpty()) { hideSuggestions(); return; }
    m_popup->setSuggestions(s);
    const QPoint topLeft = m_search->mapTo(this, QPoint(0, m_search->height() + 4));
    m_popup->setGeometry(topLeft.x(), topLeft.y(), m_search->width(), qMin(m_popup->preferredHeight(), 420));
    m_popup->raise();
    m_popup->show();
}

void MainWindow::hideSuggestions()
{
    if (m_popup) m_popup->hide();
}

void MainWindow::clearSearchUi()
{
    hideSuggestions();
    QSignalBlocker block(m_search);              // clearing is handled right here, not via the debounce
    m_search->clear();
    m_searchTimer->stop();
    if (m_model && m_model->isSearching()) { m_model->clearSearch(); refreshSidebar(); }
    updateHeader();
}

void MainWindow::selectRecording(qint64 id)
{
    clearSearchUi();
    if (!m_model) return;
    if (m_model->rowForId(id) < 0) {             // hidden by the sidebar filter: show everything so it can be found
        m_sidebarWidget->setFilter(RecordingFilter());
        m_model->reload();
    }
    const int row = m_model->rowForId(id);
    if (row >= 0) {
        m_cards->setCurrentIndex(m_model->index(row, 0));
        m_cards->scrollTo(m_model->index(row, 0));
        m_cards->setFocus();
    }
}

bool MainWindow::eventFilter(QObject* obj, QEvent* e)
{
    if (e->type() == QEvent::KeyPress && (obj == m_cards || (m_transcript && obj == m_transcript->list()))) {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Space && k->modifiers() == Qt::NoModifier && !k->isAutoRepeat() && m_playback) {
            if (obj == m_cards) {
                const qint64 id = m_cards->currentIndex().data(RecordingListModel::IdRole).toLongLong();
                if (id) togglePlayback(id);
            } else if (m_playback->recordingId() == m_transcript->recordingId()) {
                m_playback->toggle();
            } else {
                m_playback->play(m_transcript->recordingId(), 0);
            }
            return true;
        }
    }
    if (obj == m_search) {
        if (e->type() == QEvent::KeyPress) {
            auto* k = static_cast<QKeyEvent*>(e);
            const bool open = m_popup && m_popup->isVisible();
            if (open && (k->key() == Qt::Key_Down || k->key() == Qt::Key_Up)) {
                m_popup->moveSelection(k->key() == Qt::Key_Down ? 1 : -1);
                return true;
            }
            if (k->key() == Qt::Key_Escape) {
                if (open) hideSuggestions();                       // first Esc: close the suggestions
                else if (!m_search->text().isEmpty()) m_search->clear(); // second: clear the search
                return true;
            }
        } else if (e->type() == QEvent::FocusOut) {
            hideSuggestions();
        } else if (e->type() == QEvent::FocusIn && m_model && m_model->isSearching() && !m_search->text().isEmpty()) {
            showSuggestions();
        }
    }
    return QMainWindow::eventFilter(obj, e);
}

void MainWindow::setAudioPlayer(IAudioPlayer* player)
{
    if (m_playback) { m_playback->stop(); delete m_playback; m_playback = nullptr; }
    m_audio = player;
    if (!player || !m_db) { m_mini->setController(nullptr); return; }
    m_playback = new PlaybackController(*m_db, *player, this);
    m_mini->setController(m_playback);
    connect(m_playback, &PlaybackController::stateChanged, this, [this] {
        if (m_model) m_model->setPlaying(m_playback->recordingId(), m_playback->isPlaying());
    });
    connect(m_playback, &PlaybackController::recordingChanged, this, [this](qint64 id) {
        if (m_model) m_model->setPlaying(id, m_playback->isPlaying());
        if (id != 0 && m_transcript->recordingId() == id) m_transcript->setPlaybackPositionMs(m_playback->position());
        else m_transcript->clearPlayback();
    });
    connect(m_playback, &PlaybackController::positionChanged, this, [this](qint64 ms) {
        if (m_playback->recordingId() == m_transcript->recordingId()) m_transcript->setPlaybackPositionMs(ms);
    });
    connect(m_playback, &PlaybackController::errorOccurred, this, [this](const QString& m) { statusBar()->showMessage(tr("再生できません: %1").arg(m), 7000); });
}

void MainWindow::togglePlayback(qint64 id)
{
    emit playRequested(id);
    if (!m_playback) return;
    if (m_playback->recordingId() == id) m_playback->toggle();   // the one that is loaded: play <-> pause
    else m_playback->play(id, 0);
}

bool MainWindow::isTranscriptShown() const { return m_stack && m_stack->currentWidget() == m_transcript; }

void MainWindow::showTranscript(qint64 id, qint64 startMs, const QString& query)
{
    if (!m_transcript->open(id, startMs, query)) return;
    hideSuggestions();
    m_toolbar->hide();                           // the full-text screen has its own header
    m_stack->setCurrentWidget(m_transcript);
    m_transcript->list()->setFocus();
    if (m_playback && m_playback->recordingId() == id) m_transcript->setPlaybackPositionMs(m_playback->position());
    emit openTranscriptRequested(id, qMax<qint64>(0, startMs));
}

void MainWindow::showLibrary()
{
    m_stack->setCurrentIndex(0);
    m_toolbar->show();
    if (m_model) m_cards->setFocus();
}

void MainWindow::setScanService(ScanService* scan)
{
    m_scan = scan;
    if (!scan) return;
    connect(scan, &ScanService::started, this, [this](qint64) { m_scanLabel->setText(tr("◐ フォルダを走査中…")); m_scanLabel->show(); });
    connect(scan, &ScanService::finished, this, [this](qint64 id, const ScanResult& r) { onScanFinished(id, r); });
    connect(scan, &ScanService::idle, this, [this] { m_scanLabel->hide(); });
    connect(m_rescanAll, &QAction::triggered, scan, &ScanService::scanAll);
}

bool MainWindow::addFolder(const QString& path)
{
    if (!m_db) return false;
    Library lib(*m_db);
    QString err;
    const qint64 id = lib.addFolder(path, true, QString(), &err);
    if (!id) {
        statusBar()->showMessage(tr("フォルダを追加できません: %1").arg(err), 8000);
        return false;
    }
    refreshCards();                                          // the sidebar lists the new folder at once
    if (m_scan) m_scan->scanFolder(id);
    statusBar()->showMessage(tr("フォルダを追加しました: %1").arg(path), 5000);
    return true;
}

void MainWindow::onScanFinished(qint64, const ScanResult& r)
{
    if (!r.ok()) {
        statusBar()->showMessage(tr("フォルダを走査できません: %1").arg(r.error), 8000);
        return;
    }
    if (!(r.added || r.updated || r.moved || r.removed)) return;
    refreshCards();
    // new or changed recordings: analyse them by themselves unless the user turned that off
    updateModelBanner();
    if ((r.added || r.updated) && m_scheduler && m_db && Settings(*m_db).autoAnalyze()) m_scheduler->enqueueAllNew();
}

FolderDialog* MainWindow::createFolderDialog()
{
    if (!m_db || !m_scan) return nullptr;
    auto* dlg = new FolderDialog(*m_db, *m_scan, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &FolderDialog::libraryChanged, this, [this] { refreshCards(); });
    return dlg;
}

void MainWindow::showFolderDialog(qint64 selectFolderId)
{
    if (FolderDialog* dlg = createFolderDialog()) {
        if (selectFolderId) dlg->selectFolder(selectFolderId);
        dlg->exec();
    }
}

bool MainWindow::removeFolderAsked(qint64 folderId)
{
    if (!m_db) return false;
    Library lib(*m_db);
    QString path;
    for (const FolderInfo& f : lib.folders()) if (f.id == folderId) path = f.path;
    if (path.isEmpty()) return false;
    const QString text = tr("「%1」をライブラリから外します。\nこのフォルダの録音 %2 本の文字起こし・要約・タグ・メモも削除されます。\n（音声ファイルそのものは削除されません。）")
                             .arg(path).arg(lib.recordingCountInFolder(folderId));
    const bool yes = m_removeConfirm ? m_removeConfirm(tr("フォルダを外す"), text)
                                     : QMessageBox::question(this, tr("フォルダを外す"), text) == QMessageBox::Yes;
    if (!yes || !lib.removeFolder(folderId)) return false;
    refreshCards();
    statusBar()->showMessage(tr("フォルダを外しました: %1").arg(path), 5000);
    return true;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e)
{
    if (!e->mimeData()->hasUrls()) return;
    for (const QUrl& u : e->mimeData()->urls())
        if (u.isLocalFile() && QFileInfo(u.toLocalFile()).isDir()) { e->acceptProposedAction(); return; }
}

void MainWindow::dropEvent(QDropEvent* e)
{
    int added = 0;
    for (const QUrl& u : e->mimeData()->urls())
        if (u.isLocalFile() && QFileInfo(u.toLocalFile()).isDir()) added += addFolder(u.toLocalFile());
    if (added) e->acceptProposedAction();
}

void MainWindow::onFilterChanged()
{
    if (m_model) m_model->setFilter(m_sidebarWidget->filter());
    if (m_model && m_model->isSearching()) m_facetTimer->start(0);   // what is left to narrow by changed (after this click)
    updateHeader();
}

void MainWindow::refreshSidebar()
{
    if (!m_db) return;
    const QDateTime now = QDateTime::currentDateTime();
    if (m_model && m_model->isSearching()) {
        const FacetScope scope{ m_model->searchIds(), m_sidebarWidget->filter() };
        m_sidebarWidget->setData(SidebarData::load(*m_db, now, &scope));
    } else {
        m_sidebarWidget->setData(SidebarData::load(*m_db, now));
    }
}

QStringList MainWindow::conditionTexts() const
{
    QStringList out;
    for (const FilterChipBar::Chip& c : m_chips->chips()) out << c.text;
    return out;
}

void MainWindow::updateChips()
{
    const RecordingFilter& f = m_sidebarWidget->filter();
    const SidebarData& d = m_sidebarWidget->data();
    using Scope = RecordingFilter::Scope;
    using Period = RecordingFilter::Period;
    QList<FilterChipBar::Chip> chips;
    if (m_model && m_model->isSearching()) {
        QString q = m_search->text().trimmed();
        if (q.size() > 24) q = q.left(23) + QStringLiteral("…");
        chips << FilterChipBar::Chip{ FilterChipBar::Search, 0, QStringLiteral("「%1」").arg(q) };
    }
    switch (f.scope) {
    case Scope::All: break;
    case Scope::Favorites: chips << FilterChipBar::Chip{ FilterChipBar::Scope, 0, tr("お気に入り") }; break;
    case Scope::Recent: chips << FilterChipBar::Chip{ FilterChipBar::Scope, 0, tr("最近追加") }; break;
    case Scope::Unanalyzed: chips << FilterChipBar::Chip{ FilterChipBar::Scope, 0, tr("未解析") }; break;
    case Scope::Error: chips << FilterChipBar::Chip{ FilterChipBar::Scope, 0, tr("エラー") }; break;
    case Scope::Folder:
        for (const auto& fo : d.folders)
            if (fo.id == f.folderId) chips << FilterChipBar::Chip{ FilterChipBar::Scope, 0, tr("フォルダ: %1").arg(SidebarWidget::folderLabel(fo.path)) };
        break;
    }
    for (qint64 id : f.tagIds)
        for (const auto& t : d.tags)
            if (t.id == id) chips << FilterChipBar::Chip{ FilterChipBar::Tag, id, "#" + t.name };
    if (f.period == Period::ThisWeek) chips << FilterChipBar::Chip{ FilterChipBar::Period, 0, tr("今週") };
    if (f.period == Period::ThisMonth) chips << FilterChipBar::Chip{ FilterChipBar::Period, 0, tr("今月") };
    if (f.period == Period::Year) chips << FilterChipBar::Chip{ FilterChipBar::Period, 0, tr("%1年").arg(f.year) };
    m_chips->setChips(chips);
}

void MainWindow::removeCondition(int kind, qint64 key)
{
    if (kind == FilterChipBar::Search) { clearSearchUi(); return; }
    RecordingFilter f = m_sidebarWidget->filter();
    if (kind == FilterChipBar::Scope) { f.scope = RecordingFilter::Scope::All; f.folderId = 0; }
    else if (kind == FilterChipBar::Tag) f.tagIds.removeAll(key);
    else if (kind == FilterChipBar::Period) { f.period = RecordingFilter::Period::None; f.year = 0; }
    m_sidebarWidget->setFilter(f);
}

void MainWindow::clearConditions()
{
    clearSearchUi();
    m_sidebarWidget->setFilter(RecordingFilter());
    refreshSidebar();
}

void MainWindow::pickTags()
{
    const SidebarData& d = m_sidebarWidget->data();
    TagPickerDialog dlg(d.tags, m_sidebarWidget->filter().tagIds, this);
    if (dlg.exec() != QDialog::Accepted) return;
    RecordingFilter f = m_sidebarWidget->filter();
    f.tagIds = dlg.selectedTagIds();
    m_sidebarWidget->setFilter(f);
}

// Title = scope name; subtitle = matches + the extra active filters.
void MainWindow::updateHeader()
{
    const RecordingFilter& f = m_sidebarWidget->filter();
    const SidebarData& d = m_sidebarWidget->data();
    using Scope = RecordingFilter::Scope;
    QString title;
    switch (f.scope) {
    case Scope::All: title = tr("すべて"); break;
    case Scope::Favorites: title = tr("お気に入り"); break;
    case Scope::Recent: title = tr("最近追加"); break;
    case Scope::Unanalyzed: title = tr("未解析"); break;
    case Scope::Error: title = tr("エラー"); break;
    case Scope::Folder:
        for (const auto& fo : d.folders) if (fo.id == f.folderId) title = SidebarWidget::folderLabel(fo.path);
        break;
    }
    const bool searching = m_model && m_model->isSearching();
    if (searching) title = tr("検索結果");
    m_cardTitle->setText(title);

    QStringList extra;
    for (qint64 id : f.tagIds)
        for (const auto& t : d.tags) if (t.id == id) extra << "#" + t.name;
    using Period = RecordingFilter::Period;
    if (f.period == Period::ThisWeek) extra << tr("今週");
    if (f.period == Period::ThisMonth) extra << tr("今月");
    if (f.period == Period::Year) extra << tr("%1年").arg(f.year);
    const int shown = m_model ? m_model->rowCount() : 0;
    QString count = tr("%1件").arg(shown);
    if (searching) {
        count = tr("「%1」 %2件 ・ 発言 %3箇所").arg(m_search->text().trimmed()).arg(shown).arg(m_model->searchSegmentHits());
        if (shown == 0) count = tr("「%1」 該当なし").arg(m_search->text().trimmed());
        if (m_model->isSimilarSearch())                      // nothing by spelling: what is listed sounds like it
            count = tr("「%1」に一致する録音はありません ・ 読みが近い候補 %2件").arg(m_search->text().trimmed()).arg(shown);
    }
    m_cardCount->setText(extra.isEmpty() ? count : tr("%1 ・ %2").arg(count, extra.join(" ＋ ")));

    updateChips();
    updateEmptyState();
    if (!m_model) { statusBar()->showMessage(tr("0件")); return; }
    const auto c = m_model->counts();
    statusBar()->showMessage(tr("%1件 ・ 解析済 %2 ・ 解析中 %3 ・ 待機 %4 ・ エラー %5")
                                 .arg(c.total).arg(c.done).arg(c.running).arg(c.pending).arg(c.error));
}

void MainWindow::updateEmptyState()
{
    EmptyKind kind = EmptyKind::None;
    if (m_model && m_model->rowCount() == 0) {
        if (m_model->isSearching() || !m_sidebarWidget->filter().isDefault()) kind = EmptyKind::NoMatches;
        else if (m_sidebarWidget->data().folders.isEmpty()) kind = EmptyKind::NoFolders;
        else kind = EmptyKind::NoRecordings;
    }
    m_emptyKind = kind;
    switch (kind) {
    case EmptyKind::NoFolders:
        m_emptyClear->hide();
        m_emptyTitle->setText(tr("録音フォルダを登録しましょう"));
        m_emptyText->setText(tr("会議・講義・ポッドキャスト・ボイスメモなどの録音が入ったフォルダを追加すると、"
                                "自動で文字起こし・要約・キーワード抽出を行い、検索できる知識カードになります。\n\n"
                                "フォルダをこのウィンドウにドラッグ&ドロップしても追加できます。"));
        m_emptyAdd->show();
        break;
    case EmptyKind::NoRecordings:
        m_emptyClear->hide();
        m_emptyTitle->setText(tr("録音が見つかりません"));
        m_emptyText->setText(tr("登録したフォルダに対応形式（mp3 / wav / m4a / flac / ogg、動画は mp4 / mov / mkv / webm の音声）の録音がありません。\n"
                                "録音を入れるか、別のフォルダを追加してください。"));
        m_emptyAdd->show();
        break;
    case EmptyKind::NoMatches: {
        const bool narrowed = !m_sidebarWidget->filter().isDefault();       // the sidebar takes part in the emptiness
        const bool searching = m_model->isSearching();
        if (narrowed) {
            m_emptyTitle->setText(tr("条件に一致する録音がありません"));
            m_emptyText->setText(tr("%1 に一致する録音はありません。\n絞り込みを外すと、検索結果が表示されます。").arg(conditionTexts().join(QStringLiteral(" ＋ "))));
        } else {
            m_emptyTitle->setText(searching ? tr("該当する録音がありません") : tr("この条件に合う録音はありません"));
            m_emptyText->setText(searching ? tr("別のキーワードを試すか、検索を空にして一覧に戻してください。")
                                           : tr("左のサイドバーで「すべて」を選ぶか、タグ・期間の絞り込みを外してください。"));
        }
        m_emptyClear->setVisible(narrowed);
        m_emptyAdd->hide();
        break;
    }
    case EmptyKind::None: break;
    }
    m_empty->setVisible(kind != EmptyKind::None);
    m_cards->setVisible(kind == EmptyKind::None);
}

void MainWindow::buildPanes()
{
    m_splitter = new QSplitter(Qt::Horizontal);
    m_splitter->setChildrenCollapsible(false);
    m_sidebarWidget = new SidebarWidget;
    m_sidebarWidget->setTheme(m_theme);
    m_sidebar = m_sidebarWidget;
    connect(m_sidebarWidget, &SidebarWidget::filterChanged, this, [this] { onFilterChanged(); });
    connect(m_sidebarWidget, &SidebarWidget::allTagsRequested, this, [this] { pickTags(); });
    // both end in a modal dialog: not from inside the menu's own event loop
    connect(m_sidebarWidget, &SidebarWidget::folderManageRequested, this, [this](qint64 id) { QTimer::singleShot(0, this, [this, id] { showFolderDialog(id); }); });
    connect(m_sidebarWidget, &SidebarWidget::folderRemoveRequested, this, [this](qint64 id) { QTimer::singleShot(0, this, [this, id] { removeFolderAsked(id); }); });
    m_cardArea = new QWidget;
    m_cardArea->setObjectName("cardArea");
    m_cardArea->setAttribute(Qt::WA_StyledBackground, true);
    buildCardArea();
    m_detailPane = new DetailPane;
    m_detail = m_detailPane;
    connect(m_detailPane, &DetailPane::recordingEdited, this, [this](qint64 id) { onRecordingChanged(id); });
    connect(m_detailPane, &DetailPane::openTranscriptRequested, this, [this](qint64 id, qint64 ms) { showTranscript(id, ms); });
    connect(m_detailPane, &DetailPane::playRequested, this, [this](qint64 id) { togglePlayback(id); });
    connect(m_detailPane, &DetailPane::reanalyzeRequested, this, [this](qint64 id) { if (m_scheduler) m_scheduler->enqueue(id); });
    m_splitter->addWidget(m_sidebar);
    m_splitter->addWidget(m_cardArea);
    m_splitter->addWidget(m_detail);
    m_splitter->setSizes({ kSidebarWidth, width() - kSidebarWidth - kDetailWidth, kDetailWidth });
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setStretchFactor(2, 0);
    m_sidebar->setMinimumWidth(180);
    m_detail->setMinimumWidth(300);
    m_transcript = new TranscriptView;
    m_transcript->setTheme(m_theme);
    connect(m_transcript, &TranscriptView::backRequested, this, [this] { showLibrary(); });
    connect(m_transcript, &TranscriptView::recordingEdited, this, [this](qint64 id) { onRecordingChanged(id); });
    connect(m_transcript, &TranscriptView::seekRequested, this, [this](qint64 ms) {
        const qint64 id = m_transcript->recordingId();
        emit seekRequested(id, ms);
        if (m_playback) m_playback->play(id, ms);          // "click a sentence -> play from there"
    });
    m_transcript->list()->installEventFilter(this);
    m_stack = new QStackedWidget;
    m_stack->addWidget(m_splitter);      // page 0: the library
    m_stack->addWidget(m_transcript);    // page 1: the full text of one recording
    m_mini = new MiniPlayer;             // below both pages
    auto* central = new QWidget;
    auto* cl = new QVBoxLayout(central);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    cl->addWidget(m_stack, 1);
    cl->addWidget(m_mini);
    setCentralWidget(central);
}

} // namespace ake
