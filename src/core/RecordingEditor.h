#pragma once
#include <QString>

namespace ake {

class Database;
class ITokenizer;

// The user's corrections to what the analysis produced. Every call is one atomic change followed by a refresh of the
// recording's search row. User text always wins over later AI runs; removed keywords stay removed.
class RecordingEditor {
public:
    RecordingEditor(Database& db, const ITokenizer& tokenizer) : m_db(db), m_tok(tokenizer) {}

    // Empty (after trimming) text = go back to the AI result (or to the file name if there is none).
    bool setTitle(qint64 recordingId, const QString& text, QString* error = nullptr);
    bool setSummary(qint64 recordingId, const QString& text, QString* error = nullptr);
    bool revertTitle(qint64 recordingId);
    bool revertSummary(qint64 recordingId);

    // A user keyword ranks above every AI keyword. Re-adding a removed keyword lifts its block.
    bool addKeyword(qint64 recordingId, const QString& name, QString* error = nullptr);
    // Removing an AI keyword blocks it for this recording (so re-analysis does not bring it back).
    bool removeKeyword(qint64 recordingId, qint64 keywordId);

    // Corrects one sentence of the *current* transcript. The recognized text is kept (revertSegment restores it); setting
    // the text back to the original clears the correction. Afterwards the search index, keywords and AI summary are
    // rebuilt from the corrected text (the user's own title/summary/keywords are left alone).
    bool setSegmentText(qint64 recordingId, qint64 segmentId, const QString& text, QString* error = nullptr);
    bool revertSegment(qint64 recordingId, qint64 segmentId, QString* error = nullptr);

    bool setFavorite(qint64 recordingId, bool favorite);
    bool setNotes(qint64 recordingId, const QString& markdown, QString* error = nullptr); // empty deletes the note

    static constexpr int kMaxKeywordChars = 40;

private:
    bool refreshSearchRow(qint64 recordingId);
    Database& m_db;
    const ITokenizer& m_tok;
};

} // namespace ake
