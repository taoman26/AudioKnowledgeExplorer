#pragma once
#include <QList>
#include <QString>

#include "core/KeywordExtractor.h"

namespace ake {

class Database;
class ITokenizer;

// Turns a stored transcript into searchable knowledge: segment word-splitting (segments.wakati + fts_segments),
// AI keywords (keywords / recording_keywords) and the per-recording FTS row (fts_recording).
class TextIndexer {
public:
    TextIndexer(Database& db, const ITokenizer& tokenizer) : m_db(db), m_tok(tokenizer) {}

    struct Result {
        int segments = 0;
        QList<Keyword> keywords;
    };

    // Re-runnable: replaces the segment index and the *AI* keywords of this recording (user keywords stay).
    // One transaction: on failure nothing changes. Segment text honours user corrections (edited_text).
    bool index(qint64 recordingId, Result* out = nullptr, QString* error = nullptr);

    // Rebuilds the fts_recording row from title / summary / keywords / notes (call after any of them changes).
    bool rebuildRecordingFts(qint64 recordingId, QString* error = nullptr);

    int keywordCount() const; // total rows in `keywords` (diagnostics)

private:
    Database& m_db;
    const ITokenizer& m_tok;
};

} // namespace ake
