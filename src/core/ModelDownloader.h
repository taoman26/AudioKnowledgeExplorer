#pragma once
#include <QList>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;

namespace ake {

// The whisper models the app knows how to fetch (it offers two: the light default and the accurate one).
struct ModelSpec {
    QString fileName;       // "ggml-small.bin"
    qint64 approxBytes = 0; // for the "about 1.6 GB" hint; the real size comes from the server
};

namespace ModelCatalog {
QList<ModelSpec> models();                         // small (default), large-v3-turbo
const ModelSpec* find(const QString& fileName);    // nullptr for a file we cannot download
QString url(const ModelSpec& m);                   // <base>/<fileName>; base: env AKE_MODEL_BASE_URL (tests) or Hugging Face
QString path(const ModelSpec& m);                  // <models dir>/<fileName>
} // namespace ModelCatalog

// Downloads one model file into the models directory: to "<file>.part" first, renamed only when complete and
// recognisable as a ggml file, so a half-downloaded model is never mistaken for a usable one.
class ModelDownloader : public QObject {
    Q_OBJECT
public:
    explicit ModelDownloader(QObject* parent = nullptr);
    ~ModelDownloader() override;

    bool start(const ModelSpec& model);   // false if a download is already running
    void cancel();
    bool isRunning() const { return m_reply != nullptr; }
    QString fileName() const { return m_spec.fileName; }

signals:
    void progress(qint64 received, qint64 total);   // total is 0 while unknown
    void finished(bool ok, const QString& error);   // also after cancel() (ok = false, error "cancelled")

private:
    void onReadyRead();
    void onFinished();
    void finish(bool ok, const QString& error);

    QNetworkAccessManager* m_net;
    QNetworkReply* m_reply = nullptr;
    QFile* m_file = nullptr;
    ModelSpec m_spec;
    QString m_dest;
    bool m_cancelled = false;
    bool m_magicChecked = false;
    QString m_writeError;
};

} // namespace ake
