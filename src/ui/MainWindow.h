#pragma once
#include <QMainWindow>
#include <functional>

#include "ui/CardListView.h"
#include "ui/RecordingListModel.h"
#include "ui/Theme.h"

class QSplitter;
class QLineEdit;
class QStackedWidget;
class QToolBar;
class QToolButton;
class QActionGroup;
class QTimer;
class QAction;
class QLabel;
class QPushButton;

namespace ake {

class Database;
class RecordingListModel;
class CardListView;
class SidebarWidget;
class QueueStatusWidget;
class SearchSuggestPopup;
class DetailPane;
class TranscriptView;
class ScanService;
class SettingsDialog;
class FilterChipBar;
class FolderDialog;
struct ScanResult;
class IAudioPlayer;
class PlaybackController;
class MiniPlayer;
class SearchService;
class ITokenizer;
class JobScheduler;
class ModelDownloader;

// Three panes: sidebar | card area | detail pane (design.md section 4).
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
signals:
    void settingsChanged();
    // A transcript hit or a suggestion asked to open the full text at this time (the transcript view comes in F13).
    void openTranscriptRequested(qint64 recordingId, qint64 startMs);
    // The detail pane's ▶ button (playback comes in F14).
    void playRequested(qint64 recordingId);
    // A sentence was clicked in the full-text screen: play from there (the player is F14).
    void seekRequested(qint64 recordingId, qint64 startMs);

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    void applyTheme(const Theme& t);
    void applyThemeName(const QString& name);                 // system | light | dark
    // Settings dialog (analysis model/language/threads, auto analysis, scan interval, theme). Everything it changes is
    // applied at once; settingsChanged() lets the owner of the folder watcher pick up the new interval.
    SettingsDialog* createSettingsDialog();
    void showSettingsDialog();
    // Download the configured model by itself when it is missing and one of the two we know (main.cpp turns this on; tests don't)
    void setAutoDownloadModel(bool on) { m_autoDownload = on; ensureModel(); }
    ModelDownloader* modelDownloader() const { return m_downloader; }
    void applySettings();                                     // push the stored values into scheduler / theme
    QAction* settingsAction() const { return m_settingsAction; }

    QWidget* sidebar() const { return m_sidebar; }
    QWidget* cardArea() const { return m_cardArea; }
    QWidget* detailPane() const { return m_detail; }
    QLineEdit* searchBox() const { return m_search; }
    CardListView* cardView() const { return m_cards; }
    SidebarWidget* sidebarWidget() const { return m_sidebarWidget; }
    QueueStatusWidget* queueStatus() const { return m_queueStatus; }
    SearchSuggestPopup* searchPopup() const { return m_popup; }
    QToolButton* sortButton() const { return m_sortBtn; }
    QToolButton* cardsViewButton() const { return m_viewCardsBtn; }
    QToolButton* rowsViewButton() const { return m_viewRowsBtn; }
    QAction* cardsViewAction() const { return m_viewCards; }
    QAction* rowsViewAction() const { return m_viewRows; }
    // Folder registration. The scan service finds the recordings in the background; when it finishes the cards are
    // refreshed and (setting "analysis.auto") new recordings are queued for analysis. Not owned.
    using FolderChooser = std::function<QString()>;          // returns a directory or "" (cancelled); default: QFileDialog
    void setScanService(ScanService* scan);
    void setFolderChooser(FolderChooser chooser) { m_chooser = std::move(chooser); }
    bool addFolder(const QString& path);                      // register + scan; false (and a status message) on failure
    FolderDialog* createFolderDialog();                       // the dialog, parented and wired, not yet shown
    void showFolderDialog(qint64 selectFolderId = 0);         // modal; the folder preselected
    // Takes a folder out of the library (its recordings' texts go with it; the audio files stay). Asks first.
    using Confirm = std::function<bool(const QString& title, const QString& text)>;
    void setRemoveConfirm(Confirm c) { m_removeConfirm = std::move(c); }
    bool removeFolderAsked(qint64 folderId);
    QAction* rescanAction() const { return m_rescanAll; }
    QAction* addFolderAction() const { return m_addFolder; }
    QAction* folderDialogAction() const { return m_folderDialog; }
    QLabel* scanLabel() const { return m_scanLabel; }

    // Empty state shown instead of the cards: no folder yet / folders without recordings / nothing matches.
    enum class EmptyKind { None, NoFolders, NoRecordings, NoMatches };
    EmptyKind emptyKind() const { return m_emptyKind; }
    QWidget* emptyState() const { return m_empty; }
    QPushButton* emptyAddButton() const { return m_emptyAdd; }
    QPushButton* emptyClearButton() const { return m_emptyClear; }   // "絞り込みを外す" (nothing matches because of the sidebar)
    FilterChipBar* chipBar() const { return m_chips; }
    QLabel* emptyTitle() const { return m_emptyTitle; }
    QLabel* emptyText() const { return m_emptyText; }

    // Cards (knowledge cards) or Rows (dense list): the same recordings, the same selection.
    void setViewMode(CardListView::Mode mode);
    void setSortOrder(RecordingListModel::SortOrder order);
    DetailPane* detailPaneWidget() const { return m_detailPane; }
    TranscriptView* transcriptView() const { return m_transcript; }
    PlaybackController* playback() const { return m_playback; }
    MiniPlayer* miniPlayer() const { return m_mini; }
    // Enables playback (card ▶, detail pane, transcript clicks, mini player, Space). Needs the database first. Not owned.
    void setAudioPlayer(IAudioPlayer* player);
    // ▶ on a card / the detail pane: starts the recording, or pauses it if it is the one playing.
    void togglePlayback(qint64 recordingId);
    bool isTranscriptShown() const;

    // Switches to the full-text screen (query: initial in-page search) / back to the cards.
    void showTranscript(qint64 recordingId, qint64 startMs = -1, const QString& query = QString());
    void showLibrary();
    QAction* analyzeAllAction() const { return m_analyzeAll; }
    QAction* pauseAction() const { return m_pause; }
    QAction* cancelAllAction() const { return m_cancelAll; }
    RecordingListModel* recordingModel() const { return m_model; }

    // Connects the card area to a database (not owned; must outlive the window).
    void setDatabase(Database* db);
    void refreshCards();

    // Enables search (the tokenizer must be the one the index was built with). Not owned.
    void setTokenizer(const ITokenizer* tokenizer);
    // Runs the current search-box text right now (normally it runs 200 ms after typing stops).
    void runSearchNow();
    static constexpr int kSearchDelayMs = 200;

    // Connects analysis: card buttons (解析する / 再試行), menu actions and live progress. Not owned.
    void setScheduler(JobScheduler* scheduler);

    static constexpr int kSidebarWidth = 232;
    static constexpr int kDetailWidth = 400;

private:
    void buildMenus();
    void buildToolBar();
    void buildPanes();
    void buildCardArea();
    void onFilterChanged();
    void pickTags();
    void updateHeader();
    void onRecordingChanged(qint64 id);
    void onProgress(qint64 id, const QString& stage, double overall);
    void updateQueueStatus();
    bool modelMissing() const;                                // the model the scheduler would load is not on disk
    void updateModelBanner();
    void ensureModel();                                       // start the download when the model is missing (and auto-download is on)
    void onSearchTextChanged(const QString& text);
    void showSuggestions();
    void hideSuggestions();
    void clearSearchUi();
    void selectRecording(qint64 id);
    bool eventFilter(QObject* obj, QEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void onScanFinished(qint64 folderId, const ScanResult& result);
    QString titleOf(qint64 id) const;

    QSplitter* m_splitter = nullptr;
    QWidget* m_sidebar = nullptr;
    QWidget* m_cardArea = nullptr;
    QWidget* m_detail = nullptr;
    QLineEdit* m_search = nullptr;
    QLabel* m_cardTitle = nullptr;
    QLabel* m_cardCount = nullptr;
    CardListView* m_cards = nullptr;
    SidebarWidget* m_sidebarWidget = nullptr;
    Database* m_db = nullptr;
    JobScheduler* m_scheduler = nullptr;
    const ITokenizer* m_tokenizer = nullptr;
    SearchService* m_searchSvc = nullptr;
    SearchSuggestPopup* m_popup = nullptr;
    DetailPane* m_detailPane = nullptr;
    TranscriptView* m_transcript = nullptr;
    IAudioPlayer* m_audio = nullptr;
    PlaybackController* m_playback = nullptr;
    MiniPlayer* m_mini = nullptr;
    QStackedWidget* m_stack = nullptr;
    QToolBar* m_toolbar = nullptr;
    QTimer* m_searchTimer = nullptr;
    QueueStatusWidget* m_queueStatus = nullptr;
    ScanService* m_scan = nullptr;
    FolderChooser m_chooser;
    Confirm m_removeConfirm;
    QAction* m_rescanAll = nullptr;
    QAction* m_addFolder = nullptr;
    QAction* m_settingsAction = nullptr;
    QPalette m_systemPalette;
    QAction* m_folderDialog = nullptr;
    QLabel* m_scanLabel = nullptr;
    QWidget* m_empty = nullptr;
    QLabel* m_emptyTitle = nullptr;
    QLabel* m_emptyText = nullptr;
    QPushButton* m_emptyAdd = nullptr;
    QPushButton* m_emptyClear = nullptr;
    FilterChipBar* m_chips = nullptr;
    QWidget* m_modelBanner = nullptr;                          // shown while the whisper model file is missing
    QLabel* m_modelBannerText = nullptr;
    QPushButton* m_bannerSettings = nullptr;
    QPushButton* m_bannerCancel = nullptr;
    ModelDownloader* m_downloader = nullptr;
    bool m_autoDownload = false;
    qint64 m_dlReceived = 0, m_dlTotal = 0;
    QTimer* m_facetTimer = nullptr;
    EmptyKind m_emptyKind = EmptyKind::None;
    void updateEmptyState();
    void refreshSidebar();                       // counts: whole library, or inside the search result while searching
    void updateChips();
    QStringList conditionTexts() const;          // what narrows the list now, in words (chips, empty state)
    void removeCondition(int kind, qint64 key);
    void clearConditions();
    QToolButton* m_sortBtn = nullptr;
    QToolButton* m_viewCardsBtn = nullptr;
    QToolButton* m_viewRowsBtn = nullptr;
    QAction* m_viewCards = nullptr;
    QAction* m_viewRows = nullptr;
    QList<QAction*> m_sortActions;
    RecordingListModel::SortOrder m_sortOrder = RecordingListModel::SortOrder::Newest;
    QAction* m_analyzeAll = nullptr;
    QAction* m_pause = nullptr;
    QAction* m_cancelAll = nullptr;
    RecordingListModel* m_model = nullptr;
    Theme m_theme = Theme::light();
};

} // namespace ake
