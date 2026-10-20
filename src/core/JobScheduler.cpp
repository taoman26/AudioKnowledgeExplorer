#include "core/JobScheduler.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "core/Database.h"
#include "core/SqlText.h"
#include "core/KnowledgeBuilder.h"
#include "core/TranscriptRepository.h"

namespace ake {

// Lives in the worker thread; runs one job at a time (called through queued connections).
class JobScheduler::Worker : public QObject {
    Q_OBJECT
public:
    Worker(IDecoder& decoder, AnalysisPipeline::TranscriberFactory f, const std::atomic<bool>& cancel)
        : m_pipeline(decoder, std::move(f)), m_cancel(cancel) {}

public slots:
    void run(qint64 id, const QString& path, const ake::AnalysisConfig& cfg)
    {
        TranscriptResult result;
        AudioInfo info;
        QString err;
        const bool ok = m_pipeline.run(path, cfg, m_cancel,
            [&](const QString& stage, double overall) { emit progress(id, stage, overall); }, &result, &info, &err);
        emit finished(id, ok, err, result, info);
    }
signals:
    void progress(qint64 id, const QString& stage, double overall);
    void finished(qint64 id, bool ok, const QString& error, const ake::TranscriptResult& result, const ake::AudioInfo& info);

private:
    AnalysisPipeline m_pipeline;
    const std::atomic<bool>& m_cancel;
};

JobScheduler::JobScheduler(Database& db, IDecoder& decoder, AnalysisPipeline::TranscriberFactory makeTranscriber,
                           QObject* parent)
    : QObject(parent), m_db(db)
{
    qRegisterMetaType<ake::TranscriptResult>();
    qRegisterMetaType<ake::AudioInfo>();
    qRegisterMetaType<ake::AnalysisConfig>();
    m_worker = new Worker(decoder, std::move(makeTranscriber), m_cancelFlag);
    m_worker->moveToThread(&m_thread);
    connect(m_worker, &Worker::progress, this, &JobScheduler::progressChanged, Qt::QueuedConnection);
    connect(m_worker, &Worker::finished, this, &JobScheduler::onWorkerFinished, Qt::QueuedConnection);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.start();
}

JobScheduler::~JobScheduler()
{
    m_shuttingDown = true;
    m_queue.clear();
    m_cancelFlag = true; // abort a running job quickly
    m_thread.quit();
    m_thread.wait();
    // The interrupted job (if any) goes back to 'queued' so resumeInterrupted() picks it up next start.
    if (m_runningId) setStatus(m_runningId, "queued");
}

bool JobScheduler::recordingPath(qint64 id, QString* path) const
{
    QSqlQuery q(m_db.handle());
    q.prepare("SELECT abs_path FROM recordings WHERE id=?");
    q.addBindValue(id);
    if (!q.exec() || !q.next()) return false;
    *path = q.value(0).toString();
    return true;
}

bool JobScheduler::setStatus(qint64 id, const QString& status, const QString& error)
{
    QSqlQuery q(m_db.handle());
    q.prepare("UPDATE recordings SET status=?, error_msg=?, updated_at=? WHERE id=?");
    q.addBindValue(status); q.addBindValue(sqlText(error)); q.addBindValue(QDateTime::currentSecsSinceEpoch()); q.addBindValue(id);
    return q.exec() && q.numRowsAffected() > 0;
}

void JobScheduler::writeJob(qint64 id, const QString& state, double progress, const QString& error, bool failed)
{
    QSqlQuery q(m_db.handle());
    if (state == "queued") { // a fresh run replaces the history of earlier runs
        q.prepare("DELETE FROM jobs WHERE recording_id=?");
        q.addBindValue(id);
        q.exec();
        q.prepare("INSERT INTO jobs(recording_id,stage,state,progress,updated_at) VALUES(?,'transcribe','queued',0,?)");
        q.addBindValue(id); q.addBindValue(QDateTime::currentSecsSinceEpoch());
        q.exec();
        return;
    }
    q.prepare("UPDATE jobs SET state=?, progress=?, last_error=?, attempts=attempts+?, updated_at=? "
              "WHERE id=(SELECT max(id) FROM jobs WHERE recording_id=?)");
    q.addBindValue(state); q.addBindValue(progress); q.addBindValue(sqlText(error)); q.addBindValue(failed ? 1 : 0);
    q.addBindValue(QDateTime::currentSecsSinceEpoch()); q.addBindValue(id);
    q.exec();
}

bool JobScheduler::enqueue(qint64 id)
{
    QString path;
    if (!recordingPath(id, &path)) return false;
    if (id == m_runningId || m_queue.contains(id)) return true; // already scheduled
    if (!setStatus(id, "queued")) return false;
    writeJob(id, "queued", 0, QString(), false);
    m_queue.append(id);
    emit recordingChanged(id);
    emit queueChanged();
    startNext();
    return true;
}

int JobScheduler::enqueueAllNew()
{
    QList<qint64> ids;
    QSqlQuery q(m_db.handle());
    if (q.exec("SELECT id FROM recordings WHERE status='new' ORDER BY id"))
        while (q.next()) ids << q.value(0).toLongLong();
    int n = 0;
    for (qint64 id : ids) n += enqueue(id);
    return n;
}

int JobScheduler::resumeInterrupted()
{
    QList<qint64> ids;
    QSqlQuery q(m_db.handle());
    if (q.exec("SELECT id FROM recordings WHERE status IN ('queued','transcribing','analyzing') ORDER BY id"))
        while (q.next()) ids << q.value(0).toLongLong();
    int n = 0;
    for (qint64 id : ids)
        if (id != m_runningId && !m_queue.contains(id)) n += enqueue(id);
    return n;
}

bool JobScheduler::cancel(qint64 id)
{
    if (id == m_runningId) {
        m_cancelRequestedByUser = true;
        m_cancelFlag = true; // the worker returns "cancelled" shortly; onWorkerFinished resets the status
        return true;
    }
    if (m_queue.removeAll(id) == 0) return false;
    setStatus(id, "new");
    writeJob(id, "cancelled", 0, QString(), false);
    emit recordingChanged(id);
    emit queueChanged();
    return true;
}

void JobScheduler::cancelAll()
{
    const QList<qint64> queued = m_queue;
    for (qint64 id : queued) cancel(id);
    if (m_runningId) cancel(m_runningId);
}

void JobScheduler::setPaused(bool paused)
{
    if (m_paused == paused) return;
    m_paused = paused;
    emit queueChanged();
    if (!paused) startNext();
}

void JobScheduler::startNext()
{
    if (m_paused || m_shuttingDown || m_runningId != 0 || m_queue.isEmpty()) return;
    const qint64 id = m_queue.takeFirst();
    QString path;
    if (!recordingPath(id, &path)) { startNext(); return; } // recording vanished (folder removed)
    m_runningId = id;
    m_cancelFlag = false;
    m_cancelRequestedByUser = false;
    setStatus(id, "transcribing");
    writeJob(id, "running", 0, QString(), false);
    emit recordingChanged(id);
    emit queueChanged();
    QMetaObject::invokeMethod(m_worker, "run", Qt::QueuedConnection, Q_ARG(qint64, id), Q_ARG(QString, path),
                              Q_ARG(ake::AnalysisConfig, m_config));
}

void JobScheduler::onWorkerFinished(qint64 id, bool ok, const QString& error, const TranscriptResult& result,
                                    const AudioInfo& info)
{
    if (ok) {
        TranscriptRepository repo(m_db);
        QString saveErr;
        if (repo.save(id, result, &saveErr) > 0) {
            QSqlQuery q(m_db.handle()); // the decoded length is authoritative for duration/sample rate
            q.prepare("UPDATE recordings SET duration_ms=CASE WHEN ?>0 THEN ? ELSE duration_ms END,"
                      " sample_rate=CASE WHEN ?>0 THEN ? ELSE sample_rate END WHERE id=?");
            q.addBindValue(info.durationMs); q.addBindValue(info.durationMs);
            q.addBindValue(info.sampleRate); q.addBindValue(info.sampleRate); q.addBindValue(id);
            q.exec();
            QString indexErr;
            if (m_tokenizer && !KnowledgeBuilder(m_db, *m_tokenizer).build(id, nullptr, &indexErr)) {
                setStatus(id, "error", QStringLiteral("indexing failed: %1").arg(indexErr));
                writeJob(id, "failed", 0, indexErr, true);
            } else {
                setStatus(id, "done");
                writeJob(id, "done", 1.0, QString(), false);
            }
        } else {
            setStatus(id, "error", QStringLiteral("could not store transcript: %1").arg(saveErr));
            writeJob(id, "failed", 0, saveErr, true);
        }
    } else if (error == "cancelled") {
        setStatus(id, "new");
        writeJob(id, "cancelled", 0, QString(), false);
    } else {
        setStatus(id, "error", error);
        writeJob(id, "failed", 0, error, true);
    }
    m_runningId = 0;
    emit recordingChanged(id);
    emit queueChanged();
    startNext();
    if (!isBusy()) emit idle();
}

bool JobScheduler::waitUntilIdle(int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (isBusy() && !(m_paused && m_runningId == 0)) {
        if (timeoutMs >= 0 && t.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return !isBusy();
}

} // namespace ake

#include "JobScheduler.moc"
