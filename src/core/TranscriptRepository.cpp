#include "core/TranscriptRepository.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "core/Database.h"
#include "core/SqlText.h"

namespace ake {

qint64 TranscriptRepository::save(qint64 recordingId, const TranscriptResult& r, QString* error)
{
    QSqlDatabase db = m_db.handle();
    auto fail = [&](const QString& m) { if (error) *error = m; db.rollback(); return qint64(0); };
    if (!db.transaction()) { if (error) *error = db.lastError().text(); return 0; }
    QSqlQuery q(db);
    q.prepare("UPDATE transcripts SET is_current=0 WHERE recording_id=?");
    q.addBindValue(recordingId);
    if (!q.exec()) return fail(q.lastError().text());

    q.prepare("INSERT INTO transcripts(recording_id,language,model,model_version,created_at,is_current) VALUES(?,?,?,?,?,1)");
    q.addBindValue(recordingId); q.addBindValue(sqlText(r.language)); q.addBindValue(sqlText(r.model));
    q.addBindValue(sqlText(r.settings));
    q.addBindValue(QDateTime::currentSecsSinceEpoch());
    if (!q.exec()) return fail(q.lastError().text()); // also fails on an unknown recording (foreign key)
    const qint64 tid = q.lastInsertId().toLongLong();

    q.prepare("INSERT INTO segments(transcript_id,idx,start_ms,end_ms,text) VALUES(?,?,?,?,?)");
    int idx = 0;
    for (const TranscriptSegment& s : r.segments) {
        q.addBindValue(tid); q.addBindValue(idx++); q.addBindValue(s.startMs); q.addBindValue(s.endMs); q.addBindValue(s.text);
        if (!q.exec()) return fail(q.lastError().text());
    }
    if (!db.commit()) return fail(db.lastError().text());
    return tid;
}

bool TranscriptRepository::loadCurrent(qint64 recordingId, TranscriptResult* out) const
{
    QSqlQuery q(m_db.handle());
    q.prepare("SELECT id,language,model,model_version FROM transcripts WHERE recording_id=? AND is_current=1 ORDER BY id DESC LIMIT 1");
    q.addBindValue(recordingId);
    if (!q.exec() || !q.next()) return false;
    const qint64 tid = q.value(0).toLongLong();
    *out = {};
    out->language = q.value(1).toString();
    out->model = q.value(2).toString();
    out->settings = q.value(3).toString();
    q.prepare("SELECT start_ms,end_ms,COALESCE(edited_text,text) FROM segments WHERE transcript_id=? ORDER BY idx");
    q.addBindValue(tid);
    if (!q.exec()) return false;
    while (q.next())
        out->segments.append({ q.value(0).toLongLong(), q.value(1).toLongLong(), q.value(2).toString() });
    return true;
}

QList<TranscriptRepository::SegmentRow> TranscriptRepository::loadSegments(qint64 recordingId) const
{
    QList<SegmentRow> out;
    QSqlQuery q(m_db.handle());
    q.prepare("SELECT s.id, s.idx, s.start_ms, s.end_ms, s.text, COALESCE(s.edited_text, ''), s.edited_text IS NOT NULL FROM segments s"
              " JOIN transcripts t ON t.id=s.transcript_id WHERE t.recording_id=? AND t.is_current=1 ORDER BY s.idx");
    q.addBindValue(recordingId);
    if (!q.exec()) return out;
    while (q.next()) {
        SegmentRow r;
        r.id = q.value(0).toLongLong(); r.idx = q.value(1).toInt();
        r.startMs = q.value(2).toLongLong(); r.endMs = q.value(3).toLongLong();
        r.text = q.value(4).toString();
        // "edited" is decided by the column's NULL-ness in SQL; an empty correction (deleted sentence) must stay a non-null ""
        if (q.value(6).toBool()) r.editedText = q.value(5).toString().isNull() ? QStringLiteral("") : q.value(5).toString();
        out.append(r);
    }
    return out;
}

int TranscriptRepository::generationCount(qint64 recordingId) const
{
    QSqlQuery q(m_db.handle());
    q.prepare("SELECT count(*) FROM transcripts WHERE recording_id=?");
    q.addBindValue(recordingId);
    return (q.exec() && q.next()) ? q.value(0).toInt() : 0;
}

} // namespace ake
