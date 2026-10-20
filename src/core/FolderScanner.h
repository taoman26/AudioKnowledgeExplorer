#pragma once
#include <QList>
#include <QString>
#include <QStringList>

namespace ake {

struct ScanFile {
    QString absPath;
    QString relPath;
    QString format; // lower-case extension: mp3 wav m4a flac ogg mp4 mov mkv webm
    qint64 size = 0;
    qint64 mtime = 0; // seconds since epoch
};

// Pure filesystem side of scanning (no DB). Platform-specific watching lives behind IFolderWatcher later.
class FolderScanner {
public:
    static bool isSupportedExtension(const QString& ext);
    static QStringList supportedExtensions();

    // Lists supported, non-empty, non-hidden audio files under `root`.
    // `excludeGlobs` are matched (case-insensitive) against every path component of the relative path.
    // Returns false and sets *error if `root` is not a readable directory (e.g. unmounted drive).
    static bool scan(const QString& root, bool recursive, const QStringList& excludeGlobs,
                     QList<ScanFile>* out, QString* error = nullptr);

    // Cheap identity for move detection: SHA-1 over size + first 64 KiB + last 64 KiB.
    static QString fastHash(const QString& path);

    static QStringList parseGlobList(const QString& commaSeparated);
};

} // namespace ake
