#pragma once
#include <QObject>

namespace ake {

// "Something may have changed in the watched folders": the app answers by rescanning. Platform backends: polling
// (works everywhere, incl. Haiku without Node Monitor), inotify on Linux later.
class IFolderWatcher : public QObject {
    Q_OBJECT
public:
    explicit IFolderWatcher(QObject* parent = nullptr) : QObject(parent) {}
    virtual void start(int intervalSeconds) = 0;   // <= 0 stops
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
signals:
    void changeSuspected();
};

} // namespace ake
