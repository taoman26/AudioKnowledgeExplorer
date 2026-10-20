#pragma once
#include "core/ITranscriber.h"

namespace ake {

class Database;

// Stores transcripts as generations: saving a new one keeps the old rows but flips is_current.
class TranscriptRepository {
public:
    explicit TranscriptRepository(Database& db) : m_db(db) {}

    // Returns the new transcript id, or 0 with *error set. Atomic (all segments or nothing).
    qint64 save(qint64 recordingId, const TranscriptResult& result, QString* error = nullptr);
    bool loadCurrent(qint64 recordingId, TranscriptResult* out) const; // false if there is none

    // Segments of the current transcript with their ids and the user's correction (if any), for the transcript view.
    struct SegmentRow {
        qint64 id = 0;
        int idx = 0;
        qint64 startMs = 0, endMs = 0;
        QString text;        // as recognized
        QString editedText;  // the user's version; null when not edited
        bool edited() const { return !editedText.isNull(); }
        QString effective() const { return edited() ? editedText : text; }
    };
    QList<SegmentRow> loadSegments(qint64 recordingId) const;
    int generationCount(qint64 recordingId) const;

private:
    Database& m_db;
};

} // namespace ake
