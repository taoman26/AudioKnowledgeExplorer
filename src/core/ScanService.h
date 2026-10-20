#pragma once
#include <QFutureWatcher>
#include <QObject>
#include <QQueue>
#include <memory>

#include "core/Library.h"

namespace ake {

class Database;

// Runs folder scans in the background: the slow part (walk, hash, probe) on a pool thread, the database reads/writes on
// the owning thread. One folder at a time, in the order requested; asking for a folder that is already waiting or
// running is a no-op.
class ScanService : public QObject {
    Q_OBJECT
public:
    // `decoder` is optional (used for probing new files) and must be thread-safe; neither pointer is owned.
    ScanService(Database& db, IDecoder* decoder, QObject* parent = nullptr);
    ~ScanService() override;

    void scanFolder(qint64 folderId);
    void scanAll();                              // every enabled folder
    bool isScanning() const { return m_running != 0 || !m_queue.isEmpty(); }
    int pending() const { return int(m_queue.size()) + (m_running ? 1 : 0); }
    qint64 runningFolder() const { return m_running; }
    bool waitUntilIdle(int timeoutMs = -1);      // for tests/CLI: processes events meanwhile

signals:
    void started(qint64 folderId);
    void finished(qint64 folderId, const ake::ScanResult& result);   // also for failures (result.error)
    void idle();                                                     // the queue drained

private:
    void startNext();
    void onInspected();

    Database& m_db;
    IDecoder* m_decoder;
    Library m_library;
    QQueue<qint64> m_queue;
    qint64 m_running = 0;
    ScanInput m_input;
    QFutureWatcher<ScanOutput> m_watcher;
};

} // namespace ake

Q_DECLARE_METATYPE(ake::ScanResult)
