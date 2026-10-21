#include "core/SearchService.h"

#include "core/PhoneticMatch.h"

#include "core/TranscriptCleaner.h"

#include <QHash>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <algorithm>
#include <cmath>

#include "core/Database.h"
#include "core/ITokenizer.h"
#include "core/SqlText.h"

namespace ake {

namespace {
QString ftsPhrase(const QStringList& tokens, bool prefix)
{
    QString p = tokens.join(' ');
    p.replace('"', "\"\"");
    return '"' + p + '"' + (prefix ? "*" : "");
}
QString likeEscape(const QString& s)
{
    QString out = s;
    out.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_");
    return out;
}
QString idList(const QSet<qint64>& ids)
{
    QStringList l;
    for (qint64 id : ids) l << QString::number(id);
    return l.join(',');
}
} // namespace

bool SearchService::run(QSqlQuery& q) const
{
    if (q.exec()) return true;
    if (m_error.isEmpty()) m_error = q.lastError().text() + " [" + q.lastQuery().left(80) + "]";
    return false;
}

QStringList SearchService::parseTerms(const QString& query)
{
    QStringList terms;
    QString cur;
    bool quoted = false;
    auto flush = [&] {
        const QString t = cur.trimmed();
        if (!t.isEmpty()) terms << t;
        cur.clear();
    };
    for (QChar c : query) {
        if (c == '"' || c == QChar(0x201C) || c == QChar(0x201D)) {   // straight and curly quotes
            if (quoted) flush();
            else flush();
            quoted = !quoted;
        } else if (!quoted && c.isSpace()) {
            flush();
        } else {
            cur += c;
        }
    }
    flush();
    return terms;
}

QList<SearchService::Term> SearchService::buildTerms(const QString& query) const
{
    QList<Term> out;
    const QStringList raws = parseTerms(query);
    for (const QString& raw : raws) {
        Term t;
        t.raw = raw;
        QString pattern;
        const Token* prev = nullptr;
        for (const Token& tok : m_tok.tokenize(raw)) {
            if (tok.kind == Token::Kind::Punct || tok.surface.trimmed().isEmpty()) continue;
            t.tokens << tok.surface;
            // Overlapping n-grams (フォ ォル ルダ) share characters in the text: write each shared character once.
            const int shared = (prev && prev->pos >= 0 && tok.pos >= 0) ? prev->pos + prev->surface.size() - tok.pos : 0;
            if (shared > 0) pattern += QRegularExpression::escape(tok.surface.mid(shared));
            else pattern += (prev ? QStringLiteral("[^\\p{L}\\p{N}]*") : QString()) + QRegularExpression::escape(tok.surface);
            prev = &tok;                                      // (tokenize() returns a list that lives until the loop ends)
        }
        if (t.tokens.isEmpty()) continue;                    // only punctuation: nothing to look for
        // tokens may be separated by spaces or punctuation in the original text ("interview_04", "Docker Compose")
        t.highlight = QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
        out << t;
    }
    if (!out.isEmpty()) out.last().fts = ftsPhrase(out.last().tokens, true);   // last term: prefix
    for (int i = 0; i + 1 < out.size(); ++i) out[i].fts = ftsPhrase(out[i].tokens, false);
    return out;
}

int SearchService::distinctTermsIn(const QString& text, const QList<Term>& terms)
{
    int n = 0;
    for (const Term& t : terms) n += t.highlight.match(text).hasMatch();
    return n;
}

namespace {
// Segments of one transcript put together: Latin words need their space, Japanese does not
QString joinNeighbours(const QString& a, const QString& b)
{
    if (a.isEmpty() || b.isEmpty()) return a + b;
    const QChar x = a.back(), y = b.front();
    const bool gap = x.unicode() < 0x80 && x.isLetterOrNumber() && y.unicode() < 0x80 && y.isLetterOrNumber();
    return gap ? a + QLatin1Char(' ') + b : a + b;
}
} // namespace

void SearchService::makeSnippet(const QString& rawText, const QList<Term>& terms, SearchHit* hit, const QString& rawBefore, const QString& rawAfter)
{
    auto clean = [](const QString& t) { return ITokenizer::normalize(TranscriptCleaner::stripNonSpeechTags(t)).simplified(); };
    const QString mine = clean(rawText), prev = clean(rawBefore), next = clean(rawAfter);
    const QString head = joinNeighbours(prev, mine);
    const int focusFrom = int(head.size() - mine.size()), focusTo = focusFrom + int(mine.size());
    const QString text = joinNeighbours(head, next);
    int first = -1, firstLen = 0;
    for (const Term& t : terms) {                    // the first match inside the segment itself, else anywhere
        auto m = t.highlight.match(text, focusFrom);
        if (!m.hasMatch() || m.capturedStart() >= focusTo) m = t.highlight.match(text);
        if (m.hasMatch() && (first < 0 || m.capturedStart() < first)) { first = int(m.capturedStart()); firstLen = int(m.capturedLength()); }
    }
    if (first < 0) { first = focusFrom; firstLen = 0; }       // no spelled match (a match by sound): centre on the segment
    int start = std::max(0, first - kSnippetBefore);
    int end = std::min<int>(text.size(), first + firstLen + kSnippetAfter);
    // do not cut a Latin word in half
    while (start > 0 && start < first && text[start - 1].unicode() < 0x80 && text[start - 1].isLetterOrNumber() && text[start].isLetterOrNumber()) ++start;
    while (end < text.size() && end > first + firstLen && text[end].unicode() < 0x80 && text[end].isLetterOrNumber() && text[end - 1].isLetterOrNumber()) --end;
    QString snip = text.mid(start, end - start);
    const int shift = start > 0 ? 1 : 0;
    if (start > 0) snip.prepend(QChar(0x2026));
    if (end < text.size()) snip.append(QChar(0x2026));
    hit->snippet = snip;
    // all matches of all terms inside the window, ordered by position, overlapping ranges merged
    QList<QPair<int, int>> ranges;                     // [start, end)
    for (const Term& t : terms) {
        auto it = t.highlight.globalMatch(text, start);
        while (it.hasNext()) {
            const auto m = it.next();
            if (m.capturedEnd() > end) break;
            ranges.append({ int(m.capturedStart()) - start + shift, int(m.capturedEnd()) - start + shift });
        }
    }
    std::sort(ranges.begin(), ranges.end());
    hit->highlights.clear();
    for (int i = 0; i < ranges.size();) {
        int from = ranges[i].first, to = ranges[i].second;
        for (++i; i < ranges.size() && ranges[i].first <= to; ++i) to = std::max(to, ranges[i].second);
        hit->highlights << from << (to - from);
    }
}

SearchResult SearchService::search(const QString& query, int limit) const
{
    SearchResult res;
    m_error.clear();
    const QList<Term> terms = buildTerms(query);
    if (terms.isEmpty()) return res;
    for (const Term& t : terms) res.terms << t.raw;
    QSqlQuery q(m_db.handle());

    // 1) recordings that contain EVERY term somewhere (fields, transcript or file name)
    QSet<qint64> candidates;
    QSet<qint64> fileNameHits;
    QList<QSet<qint64>> exactIds;                        // per term: where it occurs, for the by-sound fallback
    for (int i = 0; i < terms.size(); ++i) {
        QSet<qint64> ids;
        q.prepare("SELECT rowid FROM fts_recording WHERE fts_recording MATCH ?");
        q.addBindValue(terms[i].fts);
        if (run(q)) while (q.next()) ids.insert(q.value(0).toLongLong());
        q.prepare("SELECT DISTINCT t.recording_id FROM fts_segments JOIN segments s ON s.id=fts_segments.rowid"
                  " JOIN transcripts t ON t.id=s.transcript_id WHERE fts_segments MATCH ? AND t.is_current=1");
        q.addBindValue(terms[i].fts);
        if (run(q)) while (q.next()) ids.insert(q.value(0).toLongLong());
        q.prepare("SELECT id FROM recordings WHERE rel_path LIKE ? ESCAPE '\\'");
        q.addBindValue('%' + likeEscape(terms[i].raw) + '%');
        if (run(q)) while (q.next()) { ids.insert(q.value(0).toLongLong()); if (i == 0) fileNameHits.insert(q.value(0).toLongLong()); }
        exactIds << ids;
        if (i == 0) candidates = ids; else candidates.intersect(ids);
    }
    if (candidates.isEmpty()) return searchSimilar(terms, exactIds, limit);
    // (file-name hits only count as such if every term matched the name; recomputed below per recording)
    QStringList ftsAny;
    for (const Term& t : terms) ftsAny << t.fts;
    const QString orQuery = ftsAny.join(" OR ");
    const QString ids = idList(candidates);

    // 2) recording-level relevance (title > keywords > summary > notes)
    QHash<qint64, double> recScore;
    q.prepare(QStringLiteral("SELECT rowid, bm25(fts_recording, %1, %2, %3, %4) FROM fts_recording WHERE fts_recording MATCH ? AND rowid IN (%5)")
                  .arg(kTitleWeight).arg(kSummaryWeight).arg(kTagWeight).arg(kNotesWeight).arg(ids));
    q.addBindValue(orQuery);
    if (run(q)) while (q.next()) recScore[q.value(0).toLongLong()] = -q.value(1).toDouble();

    // 3) transcript hits: per recording the count, the strongest few scores, and the best segment for the snippet
    struct Agg { int segs = 0; QList<double> scores; int bestTerms = -1; double bestScore = -1e9; QString bestText; qint64 bestMs = -1; qint64 bestTranscript = 0; int bestIdx = -1; };
    QHash<qint64, Agg> agg;
    q.prepare(QStringLiteral("SELECT t.recording_id, s.start_ms, COALESCE(s.edited_text, s.text), bm25(fts_segments), s.transcript_id, s.idx FROM fts_segments"
                             " JOIN segments s ON s.id=fts_segments.rowid JOIN transcripts t ON t.id=s.transcript_id"
                             " WHERE fts_segments MATCH ? AND t.is_current=1 AND t.recording_id IN (%1)").arg(ids));
    q.addBindValue(orQuery);
    if (run(q)) {
        while (q.next()) {
            Agg& a = agg[q.value(0).toLongLong()];
            const double sc = -q.value(3).toDouble();
            a.segs++;
            a.scores << sc;
            const QString text = q.value(2).toString();
            const int nt = distinctTermsIn(ITokenizer::normalize(text), terms);   // prefer a segment that has several terms
            if (nt > a.bestTerms || (nt == a.bestTerms && sc > a.bestScore)) { a.bestTerms = nt; a.bestScore = sc; a.bestText = text; a.bestMs = q.value(1).toLongLong(); a.bestTranscript = q.value(4).toLongLong(); a.bestIdx = q.value(5).toInt(); }
        }
    }

    // 4) titles/summaries/file names for recordings without a usable transcript hit
    QHash<qint64, QString> relPath, title, summary;
    q.prepare(QStringLiteral("SELECT r.id, r.rel_path,"
                          " (SELECT content FROM analyses a WHERE a.recording_id=r.id AND a.kind='title' AND a.is_current=1 ORDER BY a.id DESC LIMIT 1),"
                          " (SELECT content FROM analyses a WHERE a.recording_id=r.id AND a.kind='summary' AND a.is_current=1 ORDER BY a.id DESC LIMIT 1)"
                          " FROM recordings r WHERE r.id IN (%1)").arg(ids));
    run(q);
    while (q.next()) {
        relPath[q.value(0).toLongLong()] = q.value(1).toString();
        title[q.value(0).toLongLong()] = q.value(2).toString();
        summary[q.value(0).toLongLong()] = q.value(3).toString();
    }

    for (qint64 id : candidates) {
        SearchHit h;
        h.recordingId = id;
        const Agg a = agg.value(id);
        h.segmentHits = a.segs;
        QList<double> sc = a.scores;
        std::sort(sc.begin(), sc.end(), std::greater<double>());
        double segScore = 0;
        for (int i = 0; i < sc.size() && i < 5; ++i) segScore += sc[i];
        bool nameMatchesAll = true;
        for (const Term& t : terms) nameMatchesAll &= relPath.value(id).contains(t.raw, Qt::CaseInsensitive);
        h.score = 2.0 * recScore.value(id, 0) + segScore + (nameMatchesAll ? 3.0 : 0.0);

        const QString nTitle = ITokenizer::normalize(title.value(id)), nSummary = ITokenizer::normalize(summary.value(id));
        if (a.segs > 0) {
            h.source = SearchHit::Source::Transcript;
            h.snippetMs = a.bestMs;
            QString before, after;
            contextOf(a.bestTranscript, a.bestIdx, &before, &after);
            makeSnippet(a.bestText, terms, &h, before, after);
        } else if (distinctTermsIn(nSummary, terms) > 0) {
            h.source = SearchHit::Source::Summary;
            makeSnippet(summary.value(id), terms, &h);
        } else if (distinctTermsIn(nTitle, terms) > 0) {
            h.source = SearchHit::Source::Title;
            makeSnippet(title.value(id), terms, &h);
        } else {
            h.source = SearchHit::Source::FileName;
            makeSnippet(relPath.value(id), terms, &h);
        }
        res.hits << h;
        res.totalSegmentHits += a.segs;
    }
    std::stable_sort(res.hits.begin(), res.hits.end(), [](const SearchHit& x, const SearchHit& y) {
        if (x.score != y.score) return x.score > y.score;
        return x.recordingId > y.recordingId;
    });
    // homophones of the words typed (障害 -> 症外: the recogniser wrote another word of the same sound) come after the spelling hits
    if (m_tok.hasReadings()) {
        const SearchResult sound = searchSimilar(terms, exactIds, 0, true);
        QSet<qint64> have;
        for (const SearchHit& h : res.hits) have.insert(h.recordingId);
        for (const SearchHit& h : sound.hits) if (!have.contains(h.recordingId)) res.hits << h;
    }
    if (limit > 0 && res.hits.size() > limit) res.hits = res.hits.mid(0, limit);
    return res;
}

void SearchService::contextOf(qint64 transcriptId, int idx, QString* before, QString* after) const
{
    // segments can be a few characters long: take as many on each side as it needs to fill the snippet window
    QSqlQuery n(m_db.handle());
    n.prepare("SELECT idx, COALESCE(edited_text, text) FROM segments WHERE transcript_id=? AND idx BETWEEN ? AND ? ORDER BY idx");
    n.addBindValue(transcriptId); n.addBindValue(idx - kContextSegments); n.addBindValue(idx + kContextSegments);
    if (!n.exec()) return;
    QList<QPair<int, QString>> rows;
    while (n.next()) rows.append({ n.value(0).toInt(), n.value(1).toString() });
    for (int i = int(rows.size()) - 1; i >= 0 && before->size() < kSnippetBefore; --i)       // nearest first, going back
        if (rows[i].first < idx) *before = joinNeighbours(rows[i].second, *before);
    for (int i = 0; i < rows.size() && after->size() < kSnippetAfter; ++i)                    // and going forward
        if (rows[i].first > idx) *after = joinNeighbours(*after, rows[i].second);
}

void SearchService::fillMissingPhonetics() const
{
    QSqlQuery q(m_db.handle());
    if (!q.exec("SELECT s.id, COALESCE(s.edited_text, s.text) FROM segments s JOIN transcripts t ON t.id=s.transcript_id"
                " WHERE t.is_current=1 AND s.phonetic=''")) return;
    QList<QPair<qint64, QString>> todo;
    while (q.next()) todo.append({ q.value(0).toLongLong(), m_tok.phonetic(q.value(1).toString()) });
    if (todo.isEmpty()) return;
    QSqlDatabase db = m_db.handle();
    if (!db.transaction()) return;
    QSqlQuery w(db);
    for (const auto& t : todo) {
        w.prepare("UPDATE segments SET phonetic=? WHERE id=?");
        w.addBindValue(sqlText(t.second.isEmpty() ? QStringLiteral("-") : t.second)); w.addBindValue(t.first);   // "-": nothing readable (punctuation), not to be tried again
        w.exec();
    }
    db.commit();
}

SearchResult SearchService::searchSimilar(const QList<Term>& terms, const QList<QSet<qint64>>& exactIds, int limit, bool exactReadingOnly) const
{
    SearchResult res;
    if (!m_tok.hasReadings()) return res;                         // without readings there is nothing to compare by sound
    struct TermSound { QString reading; int allowed; };
    QList<TermSound> sounds;
    for (int i = 0; i < terms.size(); ++i) {
        TermSound t;
        t.reading = m_tok.phonetic(terms[i].raw);
        const bool kana = PhoneticMatch::isKanaOnly(t.reading);
        t.allowed = !kana ? -1 : exactReadingOnly ? (t.reading.size() >= kMinHomophoneKana ? 0 : -1)
                                                  : PhoneticMatch::allowedDistance(int(t.reading.size()));
        if (t.allowed < 0 && exactIds[i].isEmpty()) return res;   // too short to compare and found nowhere: no candidates
        sounds << t;
    }
    fillMissingPhonetics();

    struct Best { int dist = 1000; qint64 transcript = 0; int idx = -1; qint64 ms = -1; };
    QHash<qint64, QVector<Best>> best;                            // recording -> best segment per term
    QSqlQuery q(m_db.handle());
    q.prepare("SELECT t.recording_id, s.transcript_id, s.idx, s.start_ms, s.phonetic FROM segments s"
              " JOIN transcripts t ON t.id=s.transcript_id WHERE t.is_current=1 AND s.phonetic NOT IN ('', '-')");
    if (!run(q)) return res;
    while (q.next()) {
        const qint64 rec = q.value(0).toLongLong();
        const QString phon = q.value(4).toString();
        for (int i = 0; i < sounds.size(); ++i) {
            if (sounds[i].allowed < 0) continue;
            const int d = exactReadingOnly ? (phon.contains(sounds[i].reading) ? 0 : 1)   // identical reading somewhere in the segment
                                           : PhoneticMatch::distance(sounds[i].reading, phon, sounds[i].allowed);
            if (d > sounds[i].allowed) continue;
            QVector<Best>& b = best[rec];
            if (b.isEmpty()) b.resize(int(sounds.size()));
            if (d < b[i].dist) b[i] = { d, q.value(1).toLongLong(), q.value(2).toInt(), q.value(3).toLongLong() };
        }
    }
    for (auto it = best.cbegin(); it != best.cend(); ++it) {
        int total = 0;
        bool all = true;
        const Best* focus = nullptr;                              // the segment shown: that of the worst-matching term
        for (int i = 0; i < sounds.size() && all; ++i) {
            const Best& b = it.value()[i];
            if (b.idx >= 0) { total += b.dist; if (!focus || b.dist > focus->dist) focus = &b; }
            else all = exactIds[i].contains(it.key());            // spelled exactly elsewhere in this recording: fine
        }
        if (!all || !focus) continue;
        SearchHit h;
        h.recordingId = it.key();
        h.similar = true;
        h.distance = total;
        h.score = -double(total);
        h.snippetMs = focus->ms;
        QString text, before, after;
        QSqlQuery seg(m_db.handle());
        seg.prepare("SELECT COALESCE(edited_text, text) FROM segments WHERE transcript_id=? AND idx=?");
        seg.addBindValue(focus->transcript); seg.addBindValue(focus->idx);
        if (seg.exec() && seg.next()) text = seg.value(0).toString();
        contextOf(focus->transcript, focus->idx, &before, &after);
        makeSnippet(text, terms, &h, before, after);
        h.highlights.clear();                                     // positions of a sound match mean nothing in the spelling
        res.hits << h;
    }
    std::stable_sort(res.hits.begin(), res.hits.end(), [](const SearchHit& x, const SearchHit& y) {
        if (x.distance != y.distance) return x.distance < y.distance;
        return x.recordingId > y.recordingId;
    });
    if (limit > 0 && res.hits.size() > limit) res.hits = res.hits.mid(0, limit);
    res.similar = !res.hits.isEmpty();
    return res;
}

Suggestions SearchService::suggest(const QString& query, int perKind) const
{
    Suggestions out;
    m_error.clear();
    const QStringList raws = parseTerms(query);
    if (raws.isEmpty()) return out;
    QSqlQuery q(m_db.handle());

    // tags: the term being typed (the last one) against keyword names
    const QString last = ITokenizer::normalize(raws.last()).toLower();
    q.prepare("SELECT k.id, k.name, count(DISTINCT rk.recording_id) c FROM keywords k"
              " JOIN recording_keywords rk ON rk.keyword_id=k.id WHERE k.normalized LIKE ? ESCAPE '\\'"
              " GROUP BY k.id ORDER BY c DESC, k.name COLLATE NOCASE LIMIT ?");
    q.addBindValue('%' + likeEscape(last) + '%'); q.addBindValue(perKind);
    if (run(q)) while (q.next()) out.tags.append({ q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toInt() });

    // recordings: every term in the title or the file name
    QStringList conds;
    for (int i = 0; i < raws.size(); ++i)
        conds << "(COALESCE((SELECT content FROM analyses a WHERE a.recording_id=r.id AND a.kind='title' AND a.is_current=1 ORDER BY a.id DESC LIMIT 1), '')"
                 " LIKE ? ESCAPE '\\' OR r.rel_path LIKE ? ESCAPE '\\')";
    q.prepare("SELECT r.id, COALESCE((SELECT content FROM analyses a WHERE a.recording_id=r.id AND a.kind='title' AND a.is_current=1"
              " ORDER BY a.id DESC LIMIT 1), r.rel_path) FROM recordings r WHERE " + conds.join(" AND ") +
              " ORDER BY COALESCE(r.recorded_at, r.created_at) DESC LIMIT ?");
    for (const QString& raw : raws) { q.addBindValue('%' + likeEscape(raw) + '%'); q.addBindValue('%' + likeEscape(raw) + '%'); }
    q.addBindValue(perKind);
    if (run(q)) while (q.next()) out.recordings.append({ q.value(0).toLongLong(), q.value(1).toString() });

    // transcript hits (best per recording)
    const SearchResult r = search(query, 50);
    QSqlQuery t(m_db.handle());
    for (const SearchHit& h : r.hits) {
        if (out.segments.size() >= perKind) break;
        if (h.source != SearchHit::Source::Transcript) continue;
        t.prepare("SELECT COALESCE((SELECT content FROM analyses a WHERE a.recording_id=r.id AND a.kind='title' AND a.is_current=1"
                  " ORDER BY a.id DESC LIMIT 1), r.rel_path) FROM recordings r WHERE r.id=?");
        t.addBindValue(h.recordingId);
        const QString name = (t.exec() && t.next()) ? t.value(0).toString() : QString();
        out.segments.append({ h.recordingId, name, h.snippetMs, h.snippet, h.highlights });
    }
    return out;
}

} // namespace ake
