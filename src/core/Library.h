#pragma once
#include <QHash>
#include <QList>
#include <QString>

#include "core/FolderScanner.h"
#include "core/IDecoder.h"

namespace ake {

class Database;

struct FolderInfo {
    qint64 id = 0;
    QString path;
    bool enabled = true;
    bool recursive = true;
    QString excludeGlob;
    qint64 lastScanAt = 0;
};

struct ScanResult {
    int added = 0, updated = 0, moved = 0, removed = 0, unchanged = 0;
    QString error; // non-empty on failure; the DB is left untouched in that case
    bool ok() const { return error.isEmpty(); }
};

// What a scan needs to know about the database, copied out so the slow part can run without touching it.
struct ScanInput {
    struct Known { qint64 id = 0, size = 0, mtime = 0; QString hash; };
    qint64 folderId = 0;
    FolderInfo folder;
    QHash<QString, Known> known;     // abs_path -> row, for this folder
    QString error;                   // set when the folder row could not be read
};
// The slow part's result: the files on disk, and hash / metadata for the ones that are new or changed.
struct ScanFacts {
    QString hash;
    AudioInfo info;                  // zeros / invalid time when the file could not be probed
};
struct ScanOutput {
    QList<ScanFile> files;
    QHash<QString, ScanFacts> facts; // abs_path -> facts (only new/changed files)
    QString error;                   // folder not accessible (unmounted?): nothing may be deleted then
};

// Folder registration + keeping the `recordings` table in sync with the filesystem.
//
// A scan has three stages so that the slow one (walking the folder, hashing, probing every new file) can run on a
// worker thread: prepareScan() and applyScan() use the database (call them on the thread that owns it), inspect() does
// not. scanFolder() simply runs all three in the caller's thread.
class Library {
public:
    explicit Library(Database& db) : m_db(db) {}

    // Optional: when set, new/changed files are probed for duration, sample rate and creation time.
    // A file that cannot be probed is still registered (analysis will report the real error). Not owned.
    void setDecoder(IDecoder* decoder) { m_decoder = decoder; }
    IDecoder* decoder() const { return m_decoder; }

    // Returns the new folder id, or 0 with *error set (duplicate / overlapping / not a directory).
    qint64 addFolder(const QString& path, bool recursive = true, const QString& excludeGlob = QString(),
                     QString* error = nullptr);
    bool removeFolder(qint64 id);
    bool setFolderOptions(qint64 id, bool recursive, const QString& excludeGlob);
    QList<FolderInfo> folders() const;
    int recordingCountInFolder(qint64 folderId) const;

    // A missing/unreadable folder (unmounted drive) is an error and never deletes records.
    ScanResult scanFolder(qint64 folderId);

    ScanInput prepareScan(qint64 folderId) const;
    static ScanOutput inspect(const ScanInput& in, IDecoder* decoder);          // no database access: thread-safe
    ScanResult applyScan(const ScanInput& in, const ScanOutput& out);

    int recordingCount() const;

private:
    Database& m_db;
    IDecoder* m_decoder = nullptr;
};

} // namespace ake
