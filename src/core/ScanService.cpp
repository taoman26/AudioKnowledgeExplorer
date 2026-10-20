#include "core/ScanService.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QtConcurrent>

#include "core/Database.h"

namespace ake {

ScanService::ScanService(Database& db, IDecoder* decoder, QObject* parent)
    : QObject(parent), m_db(db), m_decoder(decoder), m_library(db)
{
    qRegisterMetaType<ake::ScanResult>();
    connect(&m_watcher, &QFutureWatcher<ScanOutput>::finished, this, &ScanService::onInspected);
}

ScanService::~ScanService()
{
    m_queue.clear();
    if (m_watcher.isRunning()) m_watcher.waitForFinished();   // the pool thread must be done before the decoder/DB go away
}

void ScanService::scanFolder(qint64 id)
{
    if (id == m_running || m_queue.contains(id)) return;
    m_queue.enqueue(id);
    startNext();
}

void ScanService::scanAll()
{
    for (const FolderInfo& f : m_library.folders())
        if (f.enabled) scanFolder(f.id);
}

void ScanService::startNext()
{
    if (m_running != 0 || m_queue.isEmpty()) return;
    m_running = m_queue.dequeue();
    m_input = m_library.prepareScan(m_running);              // database read: here, on the owning thread
    emit started(m_running);
    IDecoder* dec = m_decoder;
    const ScanInput in = m_input;
    m_watcher.setFuture(QtConcurrent::run([in, dec] { return Library::inspect(in, dec); }));
}

void ScanService::onInspected()
{
    const qint64 id = m_running;
    const ScanOutput out = m_watcher.result();
    const ScanResult r = m_library.applyScan(m_input, out);  // database write: back on the owning thread
    m_running = 0;
    emit finished(id, r);
    startNext();
    if (!isScanning()) emit idle();
}

bool ScanService::waitUntilIdle(int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (isScanning()) {
        if (timeoutMs >= 0 && t.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return true;
}

} // namespace ake
