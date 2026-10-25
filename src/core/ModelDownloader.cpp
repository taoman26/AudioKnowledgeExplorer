#include "core/ModelDownloader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include "core/AppPaths.h"

namespace ake {

namespace ModelCatalog {
QList<ModelSpec> models()
{
    return { { QStringLiteral("ggml-small.bin"), 488'000'000 }, { QStringLiteral("ggml-large-v3-turbo.bin"), 1'620'000'000 } };
}

const ModelSpec* find(const QString& fileName)
{
    static const QList<ModelSpec> all = models();
    for (const ModelSpec& m : all) if (m.fileName == fileName) return &m;
    return nullptr;
}

QString url(const ModelSpec& m)
{
    QString base = qEnvironmentVariable("AKE_MODEL_BASE_URL");
    if (base.isEmpty()) base = QStringLiteral("https://huggingface.co/ggerganov/whisper.cpp/resolve/main");
    while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    return base + QLatin1Char('/') + m.fileName;
}

QString path(const ModelSpec& m) { return AppPaths::modelDirectory() + QLatin1Char('/') + m.fileName; }
} // namespace ModelCatalog

ModelDownloader::ModelDownloader(QObject* parent) : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

ModelDownloader::~ModelDownloader()
{
    if (m_reply) { m_reply->disconnect(this); m_reply->abort(); m_reply->deleteLater(); m_reply = nullptr; }
    if (m_file) { const QString p = m_file->fileName(); delete m_file; m_file = nullptr; QFile::remove(p); }
}

bool ModelDownloader::start(const ModelSpec& model)
{
    if (m_reply) return false;
    m_spec = model;
    m_dest = ModelCatalog::path(model);
    m_cancelled = false;
    m_magicChecked = false;
    m_writeError.clear();
    QDir().mkpath(QFileInfo(m_dest).absolutePath());
    m_file = new QFile(m_dest + QStringLiteral(".part"));
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString err = tr("書き込めません: %1").arg(m_file->fileName());
        delete m_file; m_file = nullptr;
        emit finished(false, err);
        return true;
    }
    QNetworkRequest req{ QUrl(ModelCatalog::url(model)) };
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);   // Hugging Face redirects to a CDN
    m_reply = m_net->get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, &ModelDownloader::onReadyRead);
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) { emit progress(got, total > 0 ? total : 0); });
    connect(m_reply, &QNetworkReply::finished, this, &ModelDownloader::onFinished);
    return true;
}

void ModelDownloader::cancel()
{
    if (!m_reply) return;
    m_cancelled = true;
    m_reply->abort();   // finished() follows
}

void ModelDownloader::onReadyRead()
{
    if (!m_reply || !m_file) return;
    const QByteArray chunk = m_reply->readAll();
    if (chunk.isEmpty() || !m_writeError.isEmpty()) return;
    if (!m_magicChecked && m_reply->error() == QNetworkReply::NoError) {
        m_magicChecked = true;
        if (m_file->size() == 0 && chunk.left(4) != "lmgg") {   // ggml files start with the magic 0x67676d6c; an HTML error page does not
            m_writeError = tr("モデルファイルではありません: %1").arg(ModelCatalog::url(m_spec));
            m_reply->abort();
            return;
        }
    }
    if (m_file->write(chunk) != chunk.size()) { m_writeError = tr("書き込みに失敗しました: %1").arg(m_file->errorString()); m_reply->abort(); }
}

void ModelDownloader::onFinished()
{
    QNetworkReply* r = m_reply;
    m_reply = nullptr;
    r->deleteLater();
    if (r->bytesAvailable() > 0 && m_writeError.isEmpty()) {   // the tail that no readyRead delivered
        m_reply = r; onReadyRead(); m_reply = nullptr;
    }
    const QString part = m_file->fileName();
    const qint64 written = m_file->size();
    m_file->close();
    delete m_file; m_file = nullptr;
    const auto fail = [&](const QString& e) { QFile::remove(part); finish(false, e); };
    if (m_cancelled) return fail(tr("cancelled"));
    if (!m_writeError.isEmpty()) return fail(m_writeError);
    if (r->error() != QNetworkReply::NoError) return fail(tr("ダウンロードに失敗しました: %1").arg(r->errorString()));
    const qint64 expected = r->header(QNetworkRequest::ContentLengthHeader).toLongLong();
    if (written == 0 || (expected > 0 && written != expected)) return fail(tr("ダウンロードが途中で終わりました(%1 / %2 バイト)").arg(written).arg(expected));
    QFile::remove(m_dest);
    if (!QFile::rename(part, m_dest)) return fail(tr("保存できません: %1").arg(m_dest));
    finish(true, QString());
}

void ModelDownloader::finish(bool ok, const QString& error) { emit finished(ok, error); }

} // namespace ake
