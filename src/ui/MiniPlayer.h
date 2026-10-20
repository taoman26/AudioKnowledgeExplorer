#pragma once
#include <QWidget>

class QLabel;
class QSlider;
class QToolButton;
class QPushButton;

namespace ake {

class PlaybackController;

// Bottom bar: title and current sentence, seek bar, ±5 s, play/pause, speed, volume. Visible only while a recording is loaded.
class MiniPlayer : public QWidget {
    Q_OBJECT
public:
    explicit MiniPlayer(QWidget* parent = nullptr);
    void setController(PlaybackController* controller);   // not owned

    // --- for tests
    QToolButton* playButton() const { return m_play; }
    QToolButton* backButton() const { return m_back; }
    QToolButton* forwardButton() const { return m_fwd; }
    QToolButton* closeButton() const { return m_close; }
    QToolButton* speedButton() const { return m_speed; }
    QSlider* seekSlider() const { return m_seek; }
    QSlider* volumeSlider() const { return m_volume; }
    QString titleText() const;
    QString sentenceText() const;
    QString timeText() const;
    QString errorText() const;
    bool isDragging() const { return m_dragging; }

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void refresh();
    void refreshTime(qint64 position);
    void showError(const QString& message);
    static QString formatClock(qint64 ms);

    PlaybackController* m_ctl = nullptr;
    QLabel* m_title;
    QLabel* m_sentence;
    QLabel* m_time;
    QLabel* m_error;
    QToolButton* m_back;
    QToolButton* m_play;
    QToolButton* m_fwd;
    QToolButton* m_speed;
    QToolButton* m_close;
    QSlider* m_seek;
    QSlider* m_volume;
    QString m_fullSentence;
    bool m_dragging = false;
};

} // namespace ake
