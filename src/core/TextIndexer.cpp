#include "core/TextIndexer.h"

#include <QFileInfo>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

#include "core/Database.h"
#include "core/ITokenizer.h"
#include "core/SqlText.h"
#include "core/TranscriptCleaner.h"

namespace ake {

namespace {
// FTS5 phrase: "a b c" with embedded quotes doubled
QString phrase(const QString& wakati)
{
    QString w = wakati;
    w.replace('"', "\"\"");
    return '"' + w + '"';
}
} // namespace

bool TextIndexer::index(qint64 recordingId, Result* out, QString* error)
{
    QSqlDatabase db = m_db.handle();
    auto fail = [&](const QString& m) { if (error) *error = m; db.rollback(); return false; };
    if (!db.transaction()) { if (error) *error = db.lastError().text(); return false; }

    QSqlQuery q(db);
    q.prepare("SELECT id FROM transcripts WHERE recording_id=? AND is_current=1 ORDER BY id DESC LIMIT 1");
    q.addBindValue(recordingId);
    if (!q.exec() || !q.next()) return fail("no transcript to index");
    const qint64 transcriptId = q.value(0).toLongLong();

    // 1) segments: wakati + FTS rows
    q.prepare("SELECT id, COALESCE(edited_text, text) FROM segments WHERE transcript_id=? ORDER BY idx");
    q.addBindValue(transcriptId);
    if (!q.exec()) return fail(q.lastError().text());
    struct Seg { qint64 id; QString wakati; QString phonetic; };
    QList<Seg> segs;
    QStringList texts;
    while (q.next()) {
        const QString text = q.value(1).toString();
        texts << text;
        segs.append({ q.value(0).toLongLong(), m_tok.wakati(text),   // the search index keeps the original wording
                      m_tok.hasReadings() ? m_tok.phonetic(text) : QString() });
    }
    // keywords are counted on cleaned text, so a repeated hallucination does not become a "topic"
    QList<Token> allTokens;
    for (const QString& text : TranscriptCleaner::clean(texts)) {
        allTokens += m_tok.tokenize(text);
        allTokens.append(KeywordExtractor::sentenceBreak());
    }
    QSqlQuery w(db);
    for (const Seg& s : segs) {
        w.prepare("UPDATE segments SET wakati=?, phonetic=? WHERE id=?");
        w.addBindValue(sqlText(s.wakati)); w.addBindValue(sqlText(s.phonetic)); w.addBindValue(s.id);
        if (!w.exec()) return fail(w.lastError().text());
        w.prepare("DELETE FROM fts_segments WHERE rowid=?");
        w.addBindValue(s.id);
        if (!w.exec()) return fail(w.lastError().text());
        w.prepare("INSERT INTO fts_segments(rowid, wakati) VALUES(?,?)");
        w.addBindValue(s.id); w.addBindValue(sqlText(s.wakati));
        if (!w.exec()) return fail(w.lastError().text());
    }
    // FTS rows of superseded transcripts of this recording must not keep matching
    w.prepare("DELETE FROM fts_segments WHERE rowid IN (SELECT s.id FROM segments s JOIN transcripts t ON t.id=s.transcript_id"
              " WHERE t.recording_id=? AND t.is_current=0)");
    w.addBindValue(recordingId);
    if (!w.exec()) return fail(w.lastError().text());

    // 2) keywords, scored against the rest of the library
    CorpusStats stats;
    q.prepare("SELECT count(DISTINCT t.recording_id) FROM transcripts t WHERE t.is_current=1 AND t.recording_id<>?");
    q.addBindValue(recordingId);
    stats.otherDocuments = (q.exec() && q.next()) ? q.value(0).toInt() : 0;
    QSqlQuery dfq(db);
    stats.docFreq = [&](const QString& wakati) {
        dfq.prepare("SELECT count(DISTINCT t.recording_id) FROM fts_segments"
                    " JOIN segments s ON s.id=fts_segments.rowid JOIN transcripts t ON t.id=s.transcript_id"
                    " WHERE fts_segments MATCH ? AND t.is_current=1 AND t.recording_id<>?");
        dfq.addBindValue(phrase(wakati)); dfq.addBindValue(recordingId);
        return (dfq.exec() && dfq.next()) ? dfq.value(0).toInt() : 0;
    };
    // keywords the user removed from this recording stay removed; ask for extra candidates so the list is still full
    QSet<QString> blocked;
    q.prepare("SELECT k.normalized FROM keyword_blocks b JOIN keywords k ON k.id=b.keyword_id WHERE b.recording_id=?");
    q.addBindValue(recordingId);
    if (q.exec()) while (q.next()) blocked.insert(q.value(0).toString());
    QList<Keyword> keywords;
    for (const Keyword& k : KeywordExtractor::extract(allTokens, stats, KeywordExtractor::kDefaultTopK + int(blocked.size())))
        if (!blocked.contains(k.key) && keywords.size() < KeywordExtractor::kDefaultTopK) keywords << k;

    w.prepare("DELETE FROM recording_keywords WHERE recording_id=? AND source='ai'");
    w.addBindValue(recordingId);
    if (!w.exec()) return fail(w.lastError().text());
    for (const Keyword& k : keywords) {
        w.prepare("INSERT OR IGNORE INTO keywords(name, normalized) VALUES(?,?)");
        w.addBindValue(k.term); w.addBindValue(k.key);
        if (!w.exec()) return fail(w.lastError().text());
        // keyed by the normalized form, so "Docker" and "docker" from different recordings share one tag
        w.prepare("INSERT OR IGNORE INTO recording_keywords(recording_id, keyword_id, score, source)"
                  " SELECT ?, id, ?, 'ai' FROM keywords WHERE name=?");
        w.addBindValue(recordingId); w.addBindValue(k.score); w.addBindValue(k.term);
        if (!w.exec()) return fail(w.lastError().text());
    }
    if (!db.commit()) return fail(db.lastError().text());

    QString ftsErr;
    if (!rebuildRecordingFts(recordingId, &ftsErr)) { if (error) *error = ftsErr; return false; }
    if (out) { out->segments = segs.size(); out->keywords = keywords; }
    return true;
}

bool TextIndexer::rebuildRecordingFts(qint64 recordingId, QString* error)
{
    QSqlDatabase db = m_db.handle();
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    QSqlQuery q(db);
    q.prepare("SELECT rel_path,"
              " (SELECT content FROM analyses a WHERE a.recording_id=r.id AND a.kind='title' AND a.is_current=1 ORDER BY a.id DESC LIMIT 1),"
              " (SELECT content FROM analyses a WHERE a.recording_id=r.id AND a.kind='summary' AND a.is_current=1 ORDER BY a.id DESC LIMIT 1),"
              " (SELECT body_md FROM notes n WHERE n.recording_id=r.id)"
              " FROM recordings r WHERE r.id=?");
    q.addBindValue(recordingId);
    if (!q.exec() || !q.next()) return fail("no such recording");
    const QString title = q.value(1).isNull() ? QFileInfo(q.value(0).toString()).completeBaseName() : q.value(1).toString();
    const QString summary = q.value(2).toString();
    const QString notes = q.value(3).toString();

    QStringList tags;
    q.prepare("SELECT k.name FROM recording_keywords rk JOIN keywords k ON k.id=rk.keyword_id WHERE rk.recording_id=?"
              " ORDER BY rk.score DESC");
    q.addBindValue(recordingId);
    if (!q.exec()) return fail(q.lastError().text());
    while (q.next()) tags << m_tok.wakati(q.value(0).toString());

    if (!db.transaction()) return fail(db.lastError().text());
    QSqlQuery w(db);
    w.prepare("DELETE FROM fts_recording WHERE rowid=?");
    w.addBindValue(recordingId);
    bool ok = w.exec();
    if (ok) {
        w.prepare("INSERT INTO fts_recording(rowid,title,summary,tags,notes) VALUES(?,?,?,?,?)");
        w.addBindValue(recordingId); w.addBindValue(sqlText(m_tok.wakati(title))); w.addBindValue(sqlText(m_tok.wakati(summary)));
        w.addBindValue(sqlText(tags.join(' '))); w.addBindValue(sqlText(m_tok.wakati(notes)));
        ok = w.exec();
    }
    if (!ok) { const QString e = w.lastError().text(); db.rollback(); return fail(e); }
    return db.commit() || fail(db.lastError().text());
}

int TextIndexer::keywordCount() const
{
    QSqlQuery q(m_db.handle());
    return (q.exec("SELECT count(*) FROM keywords") && q.next()) ? q.value(0).toInt() : 0;
}

} // namespace ake
