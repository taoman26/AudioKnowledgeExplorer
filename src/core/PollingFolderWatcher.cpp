#include "core/PollingFolderWatcher.h"

namespace ake {

PollingFolderWatcher::PollingFolderWatcher(QObject* parent)
    : IFolderWatcher(parent)
{
    connect(&m_timer, &QTimer::timeout, this, &IFolderWatcher::changeSuspected);
}

void PollingFolderWatcher::startMs(int intervalMs)
{
    if (intervalMs <= 0) { m_timer.stop(); return; }
    m_timer.start(intervalMs);                     // restarts with the new interval if already running
}

} // namespace ake
