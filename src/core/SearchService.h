#pragma once
#include <QList>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class QSqlQuery;

namespace ake {

class Database;
class ITokenizer;

struct SearchHit {
    enum class Source { Transcript, Summary, Title, FileName };
    qint64 recordingId = 0;
    double score = 0;
    int segmentHits = 0;          // transcript segments containing any of the terms
    Source source = Source::Transcript;
    QString snippet;              // NFKC-normalized text around the first match, "…" at cut ends
    QVector<int> highlights;      // flat [start, length, start, length, ...] into `snippet`
    qint64 snippetMs = -1;        // start of the snippet's segment in the audio; -1 unless Source::Transcript
    bool similar = false;         // found by sound, not by spelling (SearchResult::similar): no highlights
    int distance = 0;             // similar only: edits between the typed words' readings and the segments' (0 = same sound)
};

struct SearchResult {
    QStringList terms;            // as understood (for display)
    QList<SearchHit> hits;        // best first
    int totalSegmentHits = 0;
    // Nothing matched by spelling, so these are candidates whose readings are close to the typed words (every hit has
    // `similar`). Only with a tokenizer that knows readings (MeCab).
    bool similar = false;
    bool isEmpty() const { return hits.isEmpty(); }
};

struct Suggestions {
    struct Tag { qint64 id; QString name; int count; };
    struct Recording { qint64 id; QString title; };
    struct Segment { qint64 recordingId; QString recordingTitle; qint64 startMs; QString snippet; QVector<int> highlights; };
    QList<Tag> tags;
    QList<Recording> recordings;
    QList<Segment> segments;
    bool isEmpty() const { return tags.isEmpty() && recordings.isEmpty() && segments.isEmpty(); }
};

// Full-text search over titles, summaries, keywords, notes, file names and transcripts.
//  - The query is split into terms on whitespace; "quoted text" is one phrase. Every term must occur somewhere in the
//    recording (any field, any segment): terms are ANDed across the whole recording, not within one sentence.
//  - Terms go through the same tokenizer as the index (NFKC, MeCab word-split), so 全角/半角 and case do not matter.
//  - The last term also matches as a prefix ("pod" finds Podman): search-as-you-type.
//  - When nothing matches by spelling, the words are compared by their readings (small recognisers write a similar-sounding
//    other word): see PhoneticMatch and design.md "読みの近い語を拾う検索".
class SearchService {
public:
    SearchService(Database& db, const ITokenizer& tokenizer) : m_db(db), m_tok(tokenizer) {}

    static QStringList parseTerms(const QString& query);
    SearchResult search(const QString& query, int limit = 500) const;
    Suggestions suggest(const QString& query, int perKind = 3) const;

    // First SQL error of the last search()/suggest() call (empty when all went well). Searching never throws; a
    // broken query yields fewer results, so callers/tests use this to notice real failures.
    QString lastError() const { return m_error; }

    // Weights: title > keywords > summary > notes (fts_recording column order: title, summary, tags, notes).
    static constexpr double kTitleWeight = 10, kSummaryWeight = 2, kTagWeight = 5, kNotesWeight = 1;
    static constexpr int kSnippetBefore = 30, kSnippetAfter = 70;
    static constexpr int kMinHomophoneKana = 3;  // shorter words (会/回/階) have too many homophones to add them to a result
    static constexpr int kContextSegments = 8;   // at most this many segments on each side go into a snippet

private:
    struct Term {
        QString raw;
        QStringList tokens;
        QString fts;                    // "phrase"  or  "phrase"*
        QRegularExpression highlight;   // matches the term in NFKC-normalized text (tokens separated by optional spaces)
    };
    QList<Term> buildTerms(const QString& query) const;
    // `before` / `after`: the neighbouring segments of a transcript hit, so the snippet shows the sentence around the match
    // (whisper segments are ~5 s long: the matching one alone is too short to tell whether it is the recording wanted).
    // The snippet is centred on the first match inside `text` itself.
    static void makeSnippet(const QString& text, const QList<Term>& terms, SearchHit* hit,
                            const QString& before = QString(), const QString& after = QString());
    static int distinctTermsIn(const QString& normalizedText, const QList<Term>& terms);

    // The neighbouring segments of segment `idx` (user corrections included), enough to fill the snippet window
    void contextOf(qint64 transcriptId, int idx, QString* before, QString* after) const;
    // Nothing matched by spelling: candidates by reading. `exactIds[i]` = recordings that contain term i by spelling.
    // By sound. exactReadingOnly: only readings identical to the typed word's (homophones: 障害 / 症外), only for words of at
    // least kMinHomophoneKana kana; used to add to a result that has spelling hits. Otherwise (nothing matched by spelling)
    // readings that differ a little count too (PhoneticMatch::allowedDistance).
    SearchResult searchSimilar(const QList<Term>& terms, const QList<QSet<qint64>>& exactIds, int limit, bool exactReadingOnly = false) const;
    void fillMissingPhonetics() const;   // readings of segments indexed before they existed (once, on the first use)

    bool run(QSqlQuery& q) const;   // exec + remember the first error

    Database& m_db;
    const ITokenizer& m_tok;
    mutable QString m_error;
};

} // namespace ake
