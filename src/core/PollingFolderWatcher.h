#pragma once
#include <QTimer>

#include "core/IFolderWatcher.h"

namespace ake {

// Fires changeSuspected() every interval. A rescan of an unchanged folder is cheap (stat only), so polling is fine.
class PollingFolderWatcher : public IFolderWatcher {
    Q_OBJECT
public:
    explicit PollingFolderWatcher(QObject* parent = nullptr);
    void start(int intervalSeconds) override { startMs(intervalSeconds > 0 ? intervalSeconds * 1000 : 0); }
    void startMs(int intervalMs);                  // finer grained (tests)
    void stop() override { m_timer.stop(); }
    bool isRunning() const override { return m_timer.isActive(); }
    int intervalMs() const { return m_timer.interval(); }

private:
    QTimer m_timer;
};

} // namespace ake
