#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "core/AppPaths.h"
#include "core/ModelDownloader.h"

using namespace ake;

// A tiny HTTP server: /ggml-small.bin is a fake ggml file, /ggml-large-v3-turbo.bin an HTML page, anything else 404.
class FakeServer : public QTcpServer {
public:
    QByteArray good = QByteArray("lmgg") + QByteArray(300000, 'x');
    bool truncate = false;
    FakeServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* s = nextPendingConnection()) {
                connect(s, &QTcpSocket::readyRead, s, [this, s] {
                    const QByteArray req = s->readAll();
                    QByteArray body, status = "200 OK";
                    if (req.startsWith("GET /ggml-small.bin")) body = good;
                    else if (req.startsWith("GET /ggml-large-v3-turbo.bin")) body = "<html>not a model</html>";
                    else status = "404 Not Found";
                    const qint64 declared = body.size();
                    if (truncate) body.chop(1000);
                    s->write("HTTP/1.1 " + status + "\r\nContent-Length: " + QByteArray::number(declared) + "\r\nConnection: close\r\n\r\n" + body);
                    s->disconnectFromHost();
                });
            }
        });
    }
};

class TstModelDownload : public QObject {
    Q_OBJECT
    FakeServer server;
    QString modelFile(const char* n) const { return AppPaths::modelDirectory() + "/" + n; }

    bool run(const char* name, QString* err = nullptr)
    {
        ModelDownloader d;
        QSignalSpy done(&d, &ModelDownloader::finished);
        d.start(*ModelCatalog::find(name));
        if (!done.wait(10000)) { if (err) *err = "timeout"; return false; }
        if (err) *err = done.first().at(1).toString();
        return done.first().at(0).toBool();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(server.listen(QHostAddress::LocalHost));
        qputenv("AKE_MODEL_BASE_URL", "http://127.0.0.1:" + QByteArray::number(server.serverPort()));
        QVERIFY2(AppPaths::modelDirectory().contains("ake_modeltest") || QStandardPaths::isTestModeEnabled(),
                 qPrintable(AppPaths::modelDirectory()));   // never the user's real folder
    }
    void catalogHasTwoModelsAndUrlsFollowTheBase()
    {
        QCOMPARE(ModelCatalog::models().size(), 2);
        QVERIFY(ModelCatalog::find("ggml-small.bin") && ModelCatalog::find("ggml-large-v3-turbo.bin"));
        QVERIFY(!ModelCatalog::find("ggml-tiny.bin"));
        QVERIFY(ModelCatalog::url(*ModelCatalog::find("ggml-small.bin")).startsWith("http://127.0.0.1:"));
    }
    void downloadsToThePlaceAndLeavesNoPartFile()
    {
        QString err;
        QVERIFY2(run("ggml-small.bin", &err), qPrintable(err));
        QFile f(modelFile("ggml-small.bin"));
        QVERIFY(f.exists());
        QCOMPARE(f.size(), qint64(server.good.size()));
        QVERIFY(!QFile::exists(modelFile("ggml-small.bin") + ".part"));
    }
    void aTruncatedTransferIsNotKept()
    {
        QFile::remove(modelFile("ggml-small.bin"));
        server.truncate = true;
        QString err;
        QVERIFY(!run("ggml-small.bin", &err));
        server.truncate = false;
        QVERIFY2(!err.isEmpty(), "an error text is given");
        QVERIFY(!QFile::exists(modelFile("ggml-small.bin")));
        QVERIFY(!QFile::exists(modelFile("ggml-small.bin") + ".part"));
    }
    void anHtmlPageIsRejected()
    {
        QString err;
        QVERIFY(!run("ggml-large-v3-turbo.bin", &err));
        QVERIFY2(err.contains("モデルファイルではありません"), qPrintable(err));
        QVERIFY(!QFile::exists(modelFile("ggml-large-v3-turbo.bin")));
        QVERIFY(!QFile::exists(modelFile("ggml-large-v3-turbo.bin") + ".part"));
    }
    void cancelStopsAndCleansUp()
    {
        server.good = QByteArray("lmgg") + QByteArray(1000, 'x');
        ModelDownloader d;
        QSignalSpy done(&d, &ModelDownloader::finished);
        d.start(*ModelCatalog::find("ggml-small.bin"));
        QVERIFY(d.isRunning());
        QVERIFY(!d.start(*ModelCatalog::find("ggml-small.bin")));   // only one at a time
        d.cancel();
        QVERIFY(done.count() == 1 || done.wait(5000));   // abort() may report at once
        QVERIFY(!done.first().at(0).toBool());
        QCOMPARE(done.first().at(1).toString(), QString("cancelled"));
        QVERIFY(!d.isRunning());
        QVERIFY(!QFile::exists(modelFile("ggml-small.bin") + ".part"));
    }
};

int main(int argc, char** argv)
{
#ifdef Q_OS_WIN
    QStandardPaths::setTestModeEnabled(true);          // Windows ignores XDG_DATA_HOME: Qt's test mode moves the app data folder instead
#else
    QTemporaryDir home(QDir::tempPath() + "/ake_modeltest_XXXXXX");   // the models folder is resolved from XDG_DATA_HOME
    qputenv("XDG_DATA_HOME", home.path().toUtf8());
#endif
    QCoreApplication app(argc, argv);
    app.setApplicationName("AudioKnowledgeExplorer");
    TstModelDownload t;
    return QTest::qExec(&t, argc, argv);
}
#include "tst_modeldownload.moc"
