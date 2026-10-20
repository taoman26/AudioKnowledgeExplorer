#pragma once
#include <QWidget>

class QLabel;
class QProgressBar;

namespace ake {

// Status bar widget: "解析中: name  [=====   ] 62%   待機 3". Hidden when nothing is running or queued.
class QueueStatusWidget : public QWidget {
    Q_OBJECT
public:
    explicit QueueStatusWidget(QWidget* parent = nullptr);

    void setRunning(const QString& name, const QString& stageText, double overall);
    void setQueued(int queued, bool paused);
    void setIdle();

    QString text() const;       // visible summary, for tests
    int percent() const;

private:
    void refreshVisibility();
    QLabel* m_label;
    QProgressBar* m_bar;
    QLabel* m_percent;
    QLabel* m_queued;
    bool m_running = false;
    int m_queuedCount = 0;
    bool m_paused = false;
};

} // namespace ake
