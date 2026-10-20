#pragma once
#include <QWidget>
#include <memory>

#include "core/RecordingDetail.h"
#include "ui/Theme.h"

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTimer;
class QToolButton;

namespace ake {

class Database;
class ITokenizer;
class RecordingEditor;
class FlowLayout;

// Right-hand pane (design.md section 8): title, meta, summary, keywords, memo and the actions for one recording.
// Everything the user edits goes through RecordingEditor; the pane reports changes with recordingEdited().
class DetailPane : public QWidget {
    Q_OBJECT
public:
    explicit DetailPane(QWidget* parent = nullptr);
    ~DetailPane() override;

    // Not owned. Without a tokenizer the pane is read-only.
    void setDatabase(Database* db, const ITokenizer* tokenizer);
    void setTheme(const Theme& t);

    void showRecording(qint64 id);   // 0 = nothing selected
    void reload();                   // re-read the current recording (e.g. after a job finished)
    qint64 currentId() const { return m_id; }
    bool showsRecording() const;
    void setReanalyzeEnabled(bool on);

    // --- for tests / accessibility
    QString titleText() const;
    QString summaryText() const;
    QStringList keywordTexts() const;
    QString memoText() const;
    QString metaText() const;
    QString badgeText(bool forTitle) const;
    QPushButton* openButton() const { return m_openBtn; }
    QPushButton* playButton() const { return m_playBtn; }
    QPushButton* reanalyzeButton() const { return m_reanalyzeBtn; }
    QToolButton* favoriteButton() const { return m_starBtn; }
    QToolButton* titleEditButton() const { return m_titleEditBtn; }
    QLineEdit* titleEdit() const { return m_titleEdit; }
    QToolButton* summaryEditButton() const { return m_summaryEditBtn; }
    QPlainTextEdit* summaryEdit() const { return m_summaryEdit; }
    QPushButton* summarySaveButton() const { return m_summarySave; }
    QPushButton* titleRevertButton() const { return m_titleRevert; }
    QPushButton* summaryRevertButton() const { return m_summaryRevert; }
    QPlainTextEdit* memoEdit() const { return m_memo; }
    QPushButton* addKeywordButton() const { return m_addKw; }
    QLineEdit* keywordEdit() const { return m_kwEdit; }
    QToolButton* removeButtonOfKeyword(const QString& name) const;
    void saveMemoNow();
    // The window is being closed/destroyed: from now on focus changes must not commit edits or rebuild the pane.
    void aboutToClose();
    static constexpr int kMemoSaveDelayMs = 800;

signals:
    void recordingEdited(qint64 recordingId);
    void playRequested(qint64 recordingId);
    void openTranscriptRequested(qint64 recordingId, qint64 startMs);
    void reanalyzeRequested(qint64 recordingId);

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void build();
    void showTitleEditor(bool on);
    void showSummaryEditor(bool on);
    bool summaryEditorOpen() const { return m_summaryEditor && !m_summaryEditor->isHidden(); }
    void fill();
    void rebuildChips();
    void beginTitleEdit();
    void commitTitleEdit();
    void cancelTitleEdit();
    void beginSummaryEdit();
    void endSummaryEdit(bool save);
    void beginKeywordAdd();
    void endKeywordAdd(bool add);
    void edited();
    bool canEdit() const { return m_db && m_editor; }

    Database* m_db = nullptr;
    const ITokenizer* m_tok = nullptr;
    std::unique_ptr<RecordingEditor> m_editor;
    Theme m_theme = Theme::light();
    qint64 m_id = 0;
    RecordingDetail m_detail;

    QStackedWidget* m_pages = nullptr;
    QLabel* m_empty = nullptr;
    QWidget* m_titleBox = nullptr;
    QLabel* m_title = nullptr;
    QLineEdit* m_titleEdit = nullptr;
    QToolButton* m_starBtn = nullptr;
    QToolButton* m_titleEditBtn = nullptr;
    QPushButton* m_titleRevert = nullptr;
    QLabel* m_titleBadge = nullptr;
    QLabel* m_path = nullptr;
    QLabel* m_meta = nullptr;
    QLabel* m_summaryBadge = nullptr;
    QToolButton* m_summaryEditBtn = nullptr;
    QPushButton* m_summaryRevert = nullptr;
    QWidget* m_summaryBox = nullptr;
    QLabel* m_summary = nullptr;
    QWidget* m_summaryEditor = nullptr;
    QPlainTextEdit* m_summaryEdit = nullptr;
    QPushButton* m_summarySave = nullptr;
    QWidget* m_chipHost = nullptr;
    FlowLayout* m_chips = nullptr;
    QPushButton* m_addKw = nullptr;
    QLineEdit* m_kwEdit = nullptr;
    QPlainTextEdit* m_memo = nullptr;
    QString m_memoLoaded;
    QTimer* m_memoTimer = nullptr;
    QPushButton* m_openBtn = nullptr;
    QPushButton* m_playBtn = nullptr;
    QPushButton* m_reanalyzeBtn = nullptr;
    bool m_closing = false;      // set while the pane is being destroyed: focus-out events must not commit edits then
    bool m_addingKeyword = false;
    bool m_titleEditing = false;
    bool m_reanalyzeOk = false;
};

} // namespace ake
