#include "core/RecordingDetail.h"

#include <QSqlQuery>
#include <QVariant>

#include "core/AnalysisRepository.h"
#include "core/Database.h"

namespace ake {

RecordingDetail RecordingDetail::load(Database& db, qint64 id)
{
    RecordingDetail d;
    QSqlQuery q(db.handle());
    q.prepare("SELECT rel_path, abs_path, status, error_msg, format, COALESCE(recorded_at, created_at), duration_ms, favorite FROM recordings WHERE id=?");
    q.addBindValue(id);
    if (!q.exec() || !q.next()) return d;
    d.id = id;
    d.relPath = q.value(0).toString(); d.absPath = q.value(1).toString(); d.status = q.value(2).toString();
    d.error = q.value(3).toString(); d.format = q.value(4).toString();
    d.recordedAt = QDateTime::fromSecsSinceEpoch(q.value(5).toLongLong());
    d.durationMs = q.value(6).toLongLong(); d.favorite = q.value(7).toBool();

    q.prepare("SELECT t.language, t.model, (SELECT count(*) FROM segments s WHERE s.transcript_id=t.id) FROM transcripts t"
              " WHERE t.recording_id=? AND t.is_current=1 ORDER BY t.id DESC LIMIT 1");
    q.addBindValue(id);
    if (q.exec() && q.next()) { d.language = q.value(0).toString(); d.model = q.value(1).toString(); d.segmentCount = q.value(2).toInt(); }

    AnalysisRepository repo(db);
    auto fill = [&](AnalysisRepository::Kind kind, Text* out) {
        AnalysisRepository::Entry e;
        if (repo.current(id, kind, &e) && !e.content.isEmpty()) { out->exists = true; out->content = e.content; out->isUser = e.isUser(); }
        QSqlQuery a(db.handle());   // is there an AI result to go back to?
        a.prepare("SELECT count(*) FROM analyses WHERE recording_id=? AND kind=? AND source<>'user' AND content<>''");
        a.addBindValue(id); a.addBindValue(QString::fromLatin1(AnalysisRepository::kindName(kind)));
        out->aiAvailable = a.exec() && a.next() && a.value(0).toInt() > 0;
    };
    fill(AnalysisRepository::Title, &d.title);
    fill(AnalysisRepository::Summary, &d.summary);

    q.prepare("SELECT k.id, k.name, rk.score, rk.source FROM recording_keywords rk JOIN keywords k ON k.id=rk.keyword_id"
              " WHERE rk.recording_id=? ORDER BY (rk.source='user') DESC, rk.score DESC, k.name COLLATE NOCASE");
    q.addBindValue(id);
    if (q.exec()) while (q.next()) d.keywords.append({ q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toDouble(), q.value(3).toString() == "user" });

    q.prepare("SELECT body_md FROM notes WHERE recording_id=?");
    q.addBindValue(id);
    if (q.exec() && q.next()) d.notes = q.value(0).toString();
    return d;
}

} // namespace ake
