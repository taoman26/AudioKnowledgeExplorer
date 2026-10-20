#pragma once
#include <QWidget>
#include <memory>

#include "ui/Theme.h"

class QLabel;
class QLineEdit;
class QListView;
class QPlainTextEdit;
class QPushButton;
class QToolButton;
class QWidget;

namespace ake {

class Database;
class ITokenizer;
class RecordingEditor;
class TranscriptModel;
class FlowLayout;

// Full-text screen (design.md section 9): timed sentences, in-page search, click-to-seek, playback follow and
// sentence-level corrections. Playback itself lives elsewhere: this view asks (seekRequested) and is told (setPlaybackPositionMs).
class TranscriptView : public QWidget {
    Q_OBJECT
public:
    explicit TranscriptView(QWidget* parent = nullptr);
    ~TranscriptView() override;

    void setDatabase(Database* db, const ITokenizer* tokenizer); // not owned; without a tokenizer the text is read-only
    void setTheme(const Theme& t);

    // Shows a recording. `startMs` >= 0 scrolls to that moment; `query` is put in the in-page search box.
    bool open(qint64 recordingId, qint64 startMs = -1, const QString& query = QString());
    qint64 recordingId() const { return m_id; }

    // Playback follow (the player reports its position; the current sentence is highlighted and kept in view).
    void setPlaybackPositionMs(qint64 ms);
    int playbackRow() const { return m_playRow; }
    void clearPlayback();                  // nothing is playing: drop the highlight

    // In-page search
    void setSearchText(const QString& text);
    int hitCount() const;
    int currentHitIndex() const;            // 0-based, -1 when none
    void nextHit();
    void previousHit();

    // Editing
    void beginEdit(int row);
    bool commitEdit();                      // false when nothing changed or saving failed
    void cancelEdit();
    bool revertRow(int row);                // back to the recognized text
    bool isEditing() const { return m_editRow >= 0; }

    QString copyText(bool withTimes) const;

    // --- for tests
    TranscriptModel* model() const { return m_model; }
    QListView* list() const { return m_list; }
    QLineEdit* searchEdit() const { return m_search; }
    QString counterText() const;
    QString titleText() const;
    QString infoText() const;
    QStringList keywordTexts() const;
    QToolButton* timeToggle() const { return m_timeToggle; }
    QToolButton* followToggle() const { return m_followToggle; }
    QToolButton* editToggle() const { return m_editToggle; }
    QToolButton* nextButton() const { return m_next; }
    QToolButton* prevButton() const { return m_prev; }
    QPushButton* backButton() const { return m_back; }
    QPlainTextEdit* editor() const { return m_editor; }
    QToolButton* copyButton() const { return m_copyBtn; }
    QPushButton* keywordButton(const QString& name) const;
    void aboutToClose() { m_closing = true; }

signals:
    void backRequested();
    void seekRequested(qint64 startMs);
    void recordingEdited(qint64 recordingId);

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void build();
    void applySearch();
    void goToHit(int index);
    void scrollToRow(int row, bool center);
    void onRowClicked(const QModelIndex& idx);
    void showContextMenu(const QPoint& pos);
    void updateCounter();

    Database* m_db = nullptr;
    const ITokenizer* m_tok = nullptr;
    std::unique_ptr<RecordingEditor> m_recEditor;
    Theme m_theme = Theme::light();
    qint64 m_id = 0;

    TranscriptModel* m_model = nullptr;
    QListView* m_list = nullptr;
    class TranscriptDelegate* m_delegate = nullptr;
    QPushButton* m_back = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_meta = nullptr;
    QLineEdit* m_search = nullptr;
    QLabel* m_counter = nullptr;
    QToolButton* m_prev = nullptr;
    QToolButton* m_next = nullptr;
    QToolButton* m_timeToggle = nullptr;
    QToolButton* m_followToggle = nullptr;
    QToolButton* m_editToggle = nullptr;
    QToolButton* m_copyBtn = nullptr;
    QWidget* m_chipHost = nullptr;
    FlowLayout* m_chips = nullptr;
    QLabel* m_info = nullptr;
    QPlainTextEdit* m_editor = nullptr;
    int m_editRow = -1;
    int m_playRow = -1;
    bool m_closing = false;
};

} // namespace ake
