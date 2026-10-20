#include "core/FolderScanner.h"

#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace ake {

namespace {
// Audio, and video containers (a meeting recorded as video): only their audio is read.
const char* const kExts[] = { "mp3", "wav", "m4a", "flac", "ogg", "mp4", "mov", "mkv", "webm" };
constexpr qint64 kHashChunk = 64 * 1024;
}

QStringList FolderScanner::supportedExtensions()
{
    QStringList l;
    for (const char* e : kExts) l << QString::fromLatin1(e);
    return l;
}

bool FolderScanner::isSupportedExtension(const QString& ext)
{
    return supportedExtensions().contains(ext.toLower());
}

QStringList FolderScanner::parseGlobList(const QString& s)
{
    QStringList out;
    for (const QString& g : s.split(',', Qt::SkipEmptyParts))
        if (!g.trimmed().isEmpty()) out << g.trimmed();
    return out;
}

bool FolderScanner::scan(const QString& root, bool recursive, const QStringList& excludeGlobs,
                         QList<ScanFile>* out, QString* error)
{
    out->clear();
    const QFileInfo rootInfo(root);
    if (!rootInfo.isDir() || !rootInfo.isReadable()) {
        if (error) *error = QStringLiteral("folder not accessible: %1").arg(root);
        return false;
    }
    const QDir rootDir(rootInfo.absoluteFilePath());

    QList<QRegularExpression> excludes;
    for (const QString& g : excludeGlobs)
        excludes << QRegularExpression(QRegularExpression::anchoredPattern(QRegularExpression::wildcardToRegularExpression(g)),
                                       QRegularExpression::CaseInsensitiveOption);

    QDirIterator it(rootDir.absolutePath(), QDir::Files | QDir::Readable | QDir::NoDotAndDotDot,
                    recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    while (it.hasNext()) {
        const QString abs = it.next();
        const QFileInfo fi = it.fileInfo();
        if (!isSupportedExtension(fi.suffix()) || fi.size() == 0)
            continue;
        const QString rel = rootDir.relativeFilePath(abs);
        // Skip hidden files and anything under a hidden directory (e.g. .Trash-1000).
        const QStringList parts = rel.split('/');
        bool skip = false;
        for (const QString& part : parts) {
            if (part.startsWith('.')) { skip = true; break; }
            for (const QRegularExpression& re : excludes)
                if (re.match(part).hasMatch()) { skip = true; break; }
            if (skip) break;
        }
        if (skip)
            continue;
        out->append({ abs, rel, fi.suffix().toLower(), fi.size(), fi.lastModified().toSecsSinceEpoch() });
    }
    return true;
}

QString FolderScanner::fastHash(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash h(QCryptographicHash::Sha1);
    const qint64 size = f.size();
    h.addData(QByteArray::number(size));
    h.addData(f.read(kHashChunk));
    if (size > kHashChunk * 2) {
        f.seek(size - kHashChunk);
        h.addData(f.read(kHashChunk));
    }
    return QString::fromLatin1(h.result().toHex());
}

} // namespace ake
