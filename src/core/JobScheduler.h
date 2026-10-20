#pragma once
#include <QList>
#include <QObject>
#include <QThread>
#include <atomic>
#include <memory>

#include "core/AnalysisPipeline.h"

namespace ake {

class Database;
class ITokenizer;

// Runs the analysis pipeline for queued recordings, one at a time, on a worker thread.
// All database access stays on the thread that owns the scheduler (QtSql connections are per-thread).
// Recording status: new -> queued -> transcribing -> done | error; cancelled jobs go back to new.
class JobScheduler : public QObject {
    Q_OBJECT
public:
    // `decoder` must outlive the scheduler and be thread-safe. `makeTranscriber` is called on the worker thread.
    JobScheduler(Database& db, IDecoder& decoder, AnalysisPipeline::TranscriberFactory makeTranscriber,
                 QObject* parent = nullptr);
    ~JobScheduler() override;

    // Optional: when set, a finished transcript is indexed for search and turned into keywords, summary and title. Not owned.
    void setTokenizer(const ITokenizer* tokenizer) { m_tokenizer = tokenizer; }
    void setConfig(const AnalysisConfig& cfg) { m_config = cfg; }
    const AnalysisConfig& config() const { return m_config; }

    // Queue a recording (any status except already queued/running). Returns false if it does not exist.
    bool enqueue(qint64 recordingId);
    // Queue every recording whose status is 'new' (not yet analysed). Returns how many were queued.
    int enqueueAllNew();
    // After a restart: put recordings left as queued/transcribing/analyzing back in the queue.
    int resumeInterrupted();

    // Queued job: dropped (status -> new). Running job: aborted (status -> new). Returns false if unknown.
    bool cancel(qint64 recordingId);
    void cancelAll();
    // Paused: the running job finishes, nothing new starts.
    void setPaused(bool paused);
    bool isPaused() const { return m_paused; }

    qint64 runningId() const { return m_runningId; }
    int queuedCount() const { return m_queue.size(); }
    bool isBusy() const { return m_runningId != 0 || !m_queue.isEmpty(); }

    // Blocks until nothing is queued or running (or the timeout expires); processes events meanwhile. For tests/CLI.
    bool waitUntilIdle(int timeoutMs = -1);

signals:
    void recordingChanged(qint64 recordingId);                 // status / error / metadata changed in the DB
    void progressChanged(qint64 recordingId, const QString& stage, double overall);
    void queueChanged();                                       // queue length or running job changed
    void idle();                                               // queue drained

private slots:
    void onWorkerFinished(qint64 id, bool ok, const QString& error, const ake::TranscriptResult& result,
                          const ake::AudioInfo& info);

private:
    class Worker;
    void startNext();
    bool setStatus(qint64 id, const QString& status, const QString& error = QString());
    void writeJob(qint64 id, const QString& state, double progress, const QString& error, bool failed);
    bool recordingPath(qint64 id, QString* path) const;

    Database& m_db;
    AnalysisConfig m_config;
    const ITokenizer* m_tokenizer = nullptr;
    QList<qint64> m_queue;
    qint64 m_runningId = 0;
    bool m_paused = false;
    bool m_shuttingDown = false;
    std::atomic<bool> m_cancelFlag{ false };
    bool m_cancelRequestedByUser = false;

    QThread m_thread;
    Worker* m_worker = nullptr;
};

} // namespace ake

Q_DECLARE_METATYPE(ake::TranscriptResult)
Q_DECLARE_METATYPE(ake::AudioInfo)
Q_DECLARE_METATYPE(ake::AnalysisConfig)
