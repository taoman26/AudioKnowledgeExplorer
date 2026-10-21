#include "core/Settings.h"

#include <QObject>

#include <QDir>
#include <QFileInfo>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>
#include <algorithm>

#include "core/AppPaths.h"
#include "core/Database.h"
#include "core/SqlText.h"

namespace ake {

QString Settings::value(const QString& key, const QString& fallback) const
{
    QSqlQuery q(m_db.handle());
    q.prepare("SELECT value FROM settings WHERE key=?");
    q.addBindValue(key);
    return (q.exec() && q.next()) ? q.value(0).toString() : fallback;
}

bool Settings::contains(const QString& key) const
{
    QSqlQuery q(m_db.handle());
    q.prepare("SELECT 1 FROM settings WHERE key=?");
    q.addBindValue(key);
    return q.exec() && q.next();
}

bool Settings::set(const QString& key, const QString& v)
{
    QSqlQuery q(m_db.handle());
    q.prepare("INSERT INTO settings(key, value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    q.addBindValue(key);
    q.addBindValue(sqlText(v));
    return q.exec();
}

bool Settings::remove(const QString& key)
{
    QSqlQuery q(m_db.handle());
    q.prepare("DELETE FROM settings WHERE key=?");
    q.addBindValue(key);
    return q.exec() && q.numRowsAffected() > 0;
}

int Settings::intValue(const QString& key, int fallback, int minimum, int maximum) const
{
    bool ok = false;
    const int v = value(key).toInt(&ok);
    return (ok && v >= minimum && v <= maximum) ? v : fallback;
}

bool Settings::boolValue(const QString& key, bool fallback) const
{
    const QString v = value(key).trimmed().toLower();
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return fallback;
}

AnalysisConfig Settings::analysisConfig() const
{
    AnalysisConfig c;
    c.modelPath = value(kModelPath).trimmed();                // empty: AnalysisConfig::defaultModelPath() is used later
    const QString lang = value(kLanguage, AnalysisConfig::kDefaultLanguage).trimmed().toLower();
    c.language = lang.isEmpty() ? QString::fromLatin1(AnalysisConfig::kDefaultLanguage) : lang;
    c.threads = intValue(kThreads, 0, 0, kMaxThreads);
    c.beamSize = intValue(kBeam, AnalysisConfig::kDefaultBeamSize, 0, kMaxBeam);
    c.suppressNst = boolValue(kSuppressNst, true);
    return c;
}

static QString choice(const QString& v, std::initializer_list<const char*> allowed, const char* fallback)
{
    for (const char* a : allowed) if (v == QLatin1String(a)) return v;
    return QString::fromLatin1(fallback);
}

QString Settings::theme() const { return choice(value(kTheme), { "system", "light", "dark" }, "system"); }
QString Settings::viewMode() const { return choice(value(kViewMode), { "cards", "rows" }, "cards"); }
QString Settings::sortOrder() const { return choice(value(kSortOrder), { "newest", "oldest", "longest" }, "newest"); }

QString Settings::modelDirectory()
{
    return AppPaths::modelDirectory();
}

QList<Settings::ModelInfo> Settings::installedModels(const QString& dir)
{
    QList<ModelInfo> out;
    const QFileInfoList files = QDir(dir).entryInfoList({ "ggml-*.bin" }, QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo& f : files) out.append({ f.absoluteFilePath(), f.completeBaseName(), f.size() });
    std::sort(out.begin(), out.end(), [](const ModelInfo& a, const ModelInfo& b) { return a.sizeBytes < b.sizeBytes; });
    return out;
}

QString Settings::describeModel(const QString& baseName)
{
    QString n = baseName.toLower();
    if (n.startsWith("ggml-")) n = n.mid(5);
    QString quant;
    const int q = n.indexOf("-q");
    if (q > 0 && n.mid(q + 2).at(0).isDigit()) { quant = QObject::tr("量子化版(小さく速い。精度は少し下がる)"); n = n.left(q); }
    QString what;
    if (n == "tiny" || n == "tiny.en") what = QObject::tr("最小。速いが、日本語の精度は低い");
    else if (n == "base" || n == "base.en") what = QObject::tr("軽量。日本語の精度は低め");
    else if (n == "small" || n == "small.en") what = QObject::tr("標準。軽いが、日本語では誤変換が多め(8コアで20分の録音に約7分)");
    else if (n == "medium" || n == "medium.en") what = QObject::tr("small より高精度、そのぶん重い");
    else if (n == "large-v3-turbo") what = QObject::tr("日本語の精度が高いお勧め。重い(8コアで録音とほぼ同じ時間、メモリ約2GB)");
    else if (n.startsWith("large")) what = QObject::tr("最も高精度だが、重く遅い");
    else return QString();
    return quant.isEmpty() ? what : what + QObject::tr("。") + quant;
}

} // namespace ake
