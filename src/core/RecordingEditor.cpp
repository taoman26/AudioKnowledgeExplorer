#include "core/RecordingEditor.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "core/AnalysisRepository.h"
#include "core/Database.h"
#include "core/ITokenizer.h"
#include "core/KnowledgeBuilder.h"
#include "core/SqlText.h"
#include "core/TextIndexer.h"

namespace ake {

bool RecordingEditor::refreshSearchRow(qint64 id)
{
    return TextIndexer(m_db, m_tok).rebuildRecordingFts(id);
}

bool RecordingEditor::setTitle(qint64 id, const QString& text, QString* error)
{
    AnalysisRepository repo(m_db);
    const QString t = text.simplified();
    if (t.isEmpty()) return revertTitle(id);
    return repo.saveUser(id, AnalysisRepository::Title, t, error) && refreshSearchRow(id);
}

bool RecordingEditor::setSummary(qint64 id, const QString& text, QString* error)
{
    AnalysisRepository repo(m_db);
    const QString t = text.trimmed();
    if (t.isEmpty()) return revertSummary(id);
    return repo.saveUser(id, AnalysisRepository::Summary, t, error) && refreshSearchRow(id);
}

bool RecordingEditor::revertTitle(qint64 id)
{
    return AnalysisRepository(m_db).revertToAi(id, AnalysisRepository::Title) && refreshSearchRow(id);
}

bool RecordingEditor::revertSummary(qint64 id)
{
    return AnalysisRepository(m_db).revertToAi(id, AnalysisRepository::Summary) && refreshSearchRow(id);
}

bool RecordingEditor::addKeyword(qint64 id, const QString& rawName, QString* error)
{
    const QString name = rawName.simplified().left(kMaxKeywordChars);
    if (name.isEmpty()) { if (error) *error = "empty keyword"; return false; }
    const QString key = ITokenizer::normalize(name).toLower();
    QSqlDatabase db = m_db.handle();
    auto fail = [&](const QString& m) { if (error) *error = m; db.rollback(); return false; };
    if (!db.transaction()) return fail(db.lastError().text());
    QSqlQuery q(db);
    // the keyword may exist (other recording, other spelling of the same normalized form): reuse that row
    q.prepare("SELECT id FROM keywords WHERE normalized=? ORDER BY id LIMIT 1");
    q.addBindValue(key);
    qint64 kid = (q.exec() && q.next()) ? q.value(0).toLongLong() : 0;
    if (!kid) {
        q.prepare("INSERT INTO keywords(name, normalized) VALUES(?,?)");
        q.addBindValue(name); q.addBindValue(key);
        if (!q.exec()) return fail(q.lastError().text());
        kid = q.lastInsertId().toLongLong();
    }
    q.prepare("DELETE FROM keyword_blocks WHERE recording_id=? AND keyword_id=?");
    q.addBindValue(id); q.addBindValue(kid);
    if (!q.exec()) return fail(q.lastError().text());
    // 100 puts it above any AI score; an AI keyword of the same name is promoted to a user keyword
    q.prepare("INSERT INTO recording_keywords(recording_id, keyword_id, score, source) VALUES(?,?,100,'user')"
              " ON CONFLICT(recording_id, keyword_id) DO UPDATE SET source='user', score=100");
    q.addBindValue(id); q.addBindValue(kid);
    if (!q.exec()) return fail(q.lastError().text());   // also rejects unknown recordings
    if (!db.commit()) return fail(db.lastError().text());
    return refreshSearchRow(id);
}

bool RecordingEditor::removeKeyword(qint64 id, qint64 keywordId)
{
    QSqlDatabase db = m_db.handle();
    if (!db.transaction()) return false;
    QSqlQuery q(db);
    q.prepare("SELECT source FROM recording_keywords WHERE recording_id=? AND keyword_id=?");
    q.addBindValue(id); q.addBindValue(keywordId);
    if (!q.exec() || !q.next()) { db.rollback(); return false; }
    const bool wasAi = q.value(0).toString() == "ai";
    bool ok = true;
    if (wasAi) {
        q.prepare("INSERT OR IGNORE INTO keyword_blocks(recording_id, keyword_id) VALUES(?,?)");
        q.addBindValue(id); q.addBindValue(keywordId);
        ok = q.exec();
    }
    if (ok) {
        q.prepare("DELETE FROM recording_keywords WHERE recording_id=? AND keyword_id=?");
        q.addBindValue(id); q.addBindValue(keywordId);
        ok = q.exec();
    }
    if (!ok || !db.commit()) { db.rollback(); return false; }
    return refreshSearchRow(id);
}

bool RecordingEditor::setSegmentText(qint64 id, qint64 segmentId, const QString& rawText, QString* error)
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    QSqlQuery q(m_db.handle());
    // only segments of this recording's current transcript may be edited. NULL-ness is read in SQL: comparing QVariants
    // would treat "" (a deleted sentence) and NULL (not corrected) as equal.
    q.prepare("SELECT s.text, s.edited_text IS NULL, COALESCE(s.edited_text, '') FROM segments s JOIN transcripts t ON t.id=s.transcript_id"
              " WHERE s.id=? AND t.recording_id=? AND t.is_current=1");
    q.addBindValue(segmentId); q.addBindValue(id);
    if (!q.exec() || !q.next()) return fail("no such segment in the current transcript");
    const QString original = q.value(0).toString();
    const bool wasNull = q.value(1).toBool();
    const QString before = q.value(2).toString();
    const QString text = sqlText(rawText.trimmed());   // an empty correction (sentence deleted) is "", never NULL
    const bool wantNull = (text == original);          // identical to the recognized text: no correction at all
    if (wantNull ? wasNull : (!wasNull && before == text)) return true;   // nothing changes
    q.prepare("UPDATE segments SET edited_text=? WHERE id=?");
    q.addBindValue(wantNull ? QVariant(QMetaType(QMetaType::QString)) : QVariant(text));   // typed NULL vs the text
    q.addBindValue(segmentId);
    if (!q.exec()) return fail(q.lastError().text());
    return KnowledgeBuilder(m_db, m_tok).build(id, nullptr, error);
}

bool RecordingEditor::revertSegment(qint64 id, qint64 segmentId, QString* error)
{
    QSqlQuery q(m_db.handle());
    q.prepare("SELECT s.text FROM segments s JOIN transcripts t ON t.id=s.transcript_id WHERE s.id=? AND t.recording_id=? AND t.is_current=1");
    q.addBindValue(segmentId); q.addBindValue(id);
    if (!q.exec() || !q.next()) { if (error) *error = "no such segment in the current transcript"; return false; }
    return setSegmentText(id, segmentId, q.value(0).toString(), error);
}

bool RecordingEditor::setFavorite(qint64 id, bool favorite)
{
    QSqlQuery q(m_db.handle());
    q.prepare("UPDATE recordings SET favorite=?, updated_at=? WHERE id=?");
    q.addBindValue(favorite ? 1 : 0); q.addBindValue(QDateTime::currentSecsSinceEpoch()); q.addBindValue(id);
    return q.exec() && q.numRowsAffected() > 0;
}

bool RecordingEditor::setNotes(qint64 id, const QString& markdown, QString* error)
{
    QSqlQuery q(m_db.handle());
    if (markdown.trimmed().isEmpty()) {
        q.prepare("DELETE FROM notes WHERE recording_id=?");
        q.addBindValue(id);
        if (!q.exec()) { if (error) *error = q.lastError().text(); return false; }
    } else {
        q.prepare("INSERT INTO notes(recording_id, body_md, updated_at) VALUES(?,?,?)"
                  " ON CONFLICT(recording_id) DO UPDATE SET body_md=excluded.body_md, updated_at=excluded.updated_at");
        q.addBindValue(id); q.addBindValue(markdown); q.addBindValue(QDateTime::currentSecsSinceEpoch());
        if (!q.exec()) { if (error) *error = q.lastError().text(); return false; }
    }
    return refreshSearchRow(id);
}

} // namespace ake
