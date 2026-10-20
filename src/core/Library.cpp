#include "core/Library.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "core/Database.h"
#include "core/SqlText.h"
#include "core/FolderScanner.h"
#include "core/IDecoder.h"

namespace ake {

namespace {
QString normalize(const QString& path)
{
    const QFileInfo fi(path);
    const QString canon = fi.canonicalFilePath(); // resolves symlinks; empty if missing
    return QDir::cleanPath(canon.isEmpty() ? fi.absoluteFilePath() : canon);
}
bool isInside(const QString& child, const QString& parent)
{
    return child == parent || child.startsWith(parent.endsWith('/') ? parent : parent + '/');
}

struct DbRec {
    qint64 id;
    qint64 size, mtime;
    QString hash;
};
}

qint64 Library::addFolder(const QString& path, bool recursive, const QString& excludeGlob, QString* error)
{
    auto fail = [&](const QString& m) { if (error) *error = m; return qint64(0); };
    if (!QFileInfo(path).isDir())
        return fail(QStringLiteral("not a directory: %1").arg(path));
    const QString norm = normalize(path);
    for (const FolderInfo& f : folders()) {
        if (f.path == norm)
            return fail(QStringLiteral("already registered: %1").arg(norm));
        if (isInside(norm, f.path) || isInside(f.path, norm))
            return fail(QStringLiteral("overlaps registered folder: %1").arg(f.path));
    }
    QSqlQuery q(m_db.handle());
    q.prepare("INSERT INTO folders(path, recursive, exclude_glob) VALUES(?,?,?)");
    q.addBindValue(norm);
    q.addBindValue(recursive ? 1 : 0);
    q.addBindValue(sqlText(excludeGlob));
    if (!q.exec())
        return fail(q.lastError().text());
    return q.lastInsertId().toLongLong();
}

bool Library::removeFolder(qint64 id)
{
    QSqlQuery q(m_db.handle());
    q.prepare("DELETE FROM folders WHERE id=?");
    q.addBindValue(id);
    return q.exec() && q.numRowsAffected() > 0;
}

QList<FolderInfo> Library::folders() const
{
    QList<FolderInfo> out;
    QSqlQuery q(m_db.handle());
    q.exec("SELECT id,path,enabled,recursive,exclude_glob,COALESCE(last_scan_at,0) FROM folders ORDER BY id");
    while (q.next())
        out.append({ q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toBool(), q.value(3).toBool(),
                     q.value(4).toString(), q.value(5).toLongLong() });
    return out;
}

int Library::recordingCount() const
{
    QSqlQuery q(m_db.handle());
    return (q.exec("SELECT count(*) FROM recordings") && q.next()) ? q.value(0).toInt() : 0;
}

bool Library::setFolderOptions(qint64 id, bool recursive, const QString& excludeGlob)
{
    QSqlQuery q(m_db.handle());
    q.prepare("UPDATE folders SET recursive=?, exclude_glob=? WHERE id=?");
    q.addBindValue(recursive ? 1 : 0); q.addBindValue(sqlText(excludeGlob)); q.addBindValue(id);
    return q.exec() && q.numRowsAffected() > 0;
}

int Library::recordingCountInFolder(qint64 folderId) const
{
    QSqlQuery q(m_db.handle());
    q.prepare("SELECT count(*) FROM recordings WHERE folder_id=?");
    q.addBindValue(folderId);
    return (q.exec() && q.next()) ? q.value(0).toInt() : 0;
}

ScanInput Library::prepareScan(qint64 folderId) const
{
    ScanInput in;
    in.folderId = folderId;
    bool found = false;
    for (const FolderInfo& f : folders())
        if (f.id == folderId) { in.folder = f; found = true; }
    if (!found) { in.error = QStringLiteral("no such folder id %1").arg(folderId); return in; }
    QSqlQuery q(m_db.handle());
    q.prepare("SELECT id,abs_path,file_size,mtime,content_hash FROM recordings WHERE folder_id=?");
    q.addBindValue(folderId);
    if (!q.exec()) { in.error = q.lastError().text(); return in; }
    while (q.next())
        in.known.insert(q.value(1).toString(), { q.value(0).toLongLong(), q.value(2).toLongLong(), q.value(3).toLongLong(), q.value(4).toString() });
    return in;
}

ScanOutput Library::inspect(const ScanInput& in, IDecoder* decoder)
{
    ScanOutput out;
    if (!in.error.isEmpty()) { out.error = in.error; return out; }
    if (!FolderScanner::scan(in.folder.path, in.folder.recursive, FolderScanner::parseGlobList(in.folder.excludeGlob),
                             &out.files, &out.error))
        return out;                                  // keep existing records: the drive may just be unmounted

    // Which known files are gone from disk? New files that match one of them (same hash and size) are moves: they keep the
    // old row, so they need no probing. Claims are counted in file order, exactly as applyScan() will make them.
    QSet<QString> onDisk;
    for (const ScanFile& f : out.files) onDisk.insert(f.absPath);
    QHash<QString, int> missingByKey;
    for (auto it = in.known.cbegin(); it != in.known.cend(); ++it)
        if (!onDisk.contains(it.key())) missingByKey[it.value().hash + ':' + QString::number(it.value().size)]++;

    for (const ScanFile& f : out.files) {
        const auto k = in.known.constFind(f.absPath);
        const bool changed = k != in.known.cend() && !(k->size == f.size && k->mtime == f.mtime);
        const bool fresh = k == in.known.cend();
        if (!changed && !fresh) continue;            // unchanged: nothing to learn
        ScanFacts facts;
        facts.hash = FolderScanner::fastHash(f.absPath);
        bool isMove = false;
        if (fresh && !facts.hash.isEmpty()) {
            int& n = missingByKey[facts.hash + ':' + QString::number(f.size)];
            if (n > 0) { --n; isMove = true; }
        }
        if (!isMove && decoder) decoder->probe(f.absPath, &facts.info);   // failure leaves zeros / invalid time
        out.facts.insert(f.absPath, facts);
    }
    return out;
}

ScanResult Library::applyScan(const ScanInput& in, const ScanOutput& out)
{
    ScanResult r;
    if (!in.error.isEmpty()) { r.error = in.error; return r; }
    if (!out.error.isEmpty()) { r.error = out.error; return r; }
    const qint64 folderId = in.folderId;

    QSqlDatabase db = m_db.handle();
    QHash<QString, ScanInput::Known> existing = in.known;     // whatever remains after matching is missing from disk
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (!db.transaction()) { r.error = db.lastError().text(); return r; }
    auto rollback = [&](const QString& e) { r.error = e; db.rollback(); r.added = r.updated = r.moved = r.removed = 0; return r; };

    QList<ScanFile> fresh; // on disk, not in DB yet
    for (const ScanFile& f : out.files) {
        auto it = existing.find(f.absPath);
        if (it == existing.end()) { fresh.append(f); continue; }
        const ScanInput::Known rec = it.value();
        existing.erase(it);
        if (rec.size == f.size && rec.mtime == f.mtime) { r.unchanged++; continue; }
        // Content changed in place: needs re-analysis.
        const ScanFacts facts = out.facts.value(f.absPath);
        QSqlQuery u(db);
        u.prepare("UPDATE recordings SET file_size=?, mtime=?, content_hash=?, status='new', error_msg='',"
                  " duration_ms=?, sample_rate=?, recorded_at=COALESCE(?, recorded_at), updated_at=? WHERE id=?");
        u.addBindValue(f.size); u.addBindValue(f.mtime); u.addBindValue(facts.hash);
        u.addBindValue(facts.info.durationMs); u.addBindValue(facts.info.sampleRate);
        u.addBindValue(facts.info.creationTime.isValid() ? QVariant(facts.info.creationTime.toSecsSinceEpoch()) : QVariant());
        u.addBindValue(now); u.addBindValue(rec.id);
        if (!u.exec()) return rollback(u.lastError().text());
        r.updated++;
    }

    // Missing records, indexed by (hash,size) so renamed/moved files keep their id and analysis results.
    QHash<QString, QList<ScanInput::Known>> missingByKey;
    for (auto it = existing.begin(); it != existing.end(); ++it)
        missingByKey[it.value().hash + ':' + QString::number(it.value().size)].append(it.value());

    for (const ScanFile& f : fresh) {
        const ScanFacts facts = out.facts.value(f.absPath);
        const QString key = facts.hash + ':' + QString::number(f.size);
        QSqlQuery u(db);
        if (auto mit = missingByKey.find(key); mit != missingByKey.end() && !mit->isEmpty() && !facts.hash.isEmpty()) {
            const ScanInput::Known rec = mit->takeFirst();
            u.prepare("UPDATE recordings SET abs_path=?, rel_path=?, mtime=?, updated_at=? WHERE id=?");
            u.addBindValue(f.absPath); u.addBindValue(f.relPath); u.addBindValue(f.mtime);
            u.addBindValue(now); u.addBindValue(rec.id);
            if (!u.exec()) return rollback(u.lastError().text());
            r.moved++;
            continue;
        }
        u.prepare("INSERT INTO recordings(folder_id,rel_path,abs_path,file_size,mtime,content_hash,format,duration_ms,sample_rate,"
                  "recorded_at,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
        u.addBindValue(folderId); u.addBindValue(f.relPath); u.addBindValue(f.absPath);
        u.addBindValue(f.size); u.addBindValue(f.mtime); u.addBindValue(facts.hash); u.addBindValue(f.format);
        u.addBindValue(facts.info.durationMs); u.addBindValue(facts.info.sampleRate);
        u.addBindValue(facts.info.creationTime.isValid() ? facts.info.creationTime.toSecsSinceEpoch() : f.mtime); // tag beats file time
        u.addBindValue(now); u.addBindValue(now);
        if (!u.exec()) return rollback(u.lastError().text());
        r.added++;
    }

    // Anything still unclaimed in missingByKey is really gone.
    for (const QList<ScanInput::Known>& list : missingByKey)
        for (const ScanInput::Known& rec : list) {
            QSqlQuery d(db);
            d.prepare("DELETE FROM recordings WHERE id=?");
            d.addBindValue(rec.id);
            if (!d.exec()) return rollback(d.lastError().text());
            r.removed++;
        }

    QSqlQuery s(db);
    s.prepare("UPDATE folders SET last_scan_at=? WHERE id=?");
    s.addBindValue(now); s.addBindValue(folderId);
    if (!s.exec()) return rollback(s.lastError().text());
    if (!db.commit()) return rollback(db.lastError().text());
    return r;
}

ScanResult Library::scanFolder(qint64 folderId)
{
    const ScanInput in = prepareScan(folderId);
    return applyScan(in, inspect(in, m_decoder));
}

} // namespace ake
