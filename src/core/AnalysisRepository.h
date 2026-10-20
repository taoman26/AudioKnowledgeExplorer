#pragma once
#include <QString>

namespace ake {

class Database;

// Title / summary per recording. AI results never overwrite a user's text; user text always wins.
class AnalysisRepository {
public:
    enum Kind { Title, Summary };
    struct Entry {
        QString content;
        QString source;   // "ai-extractive" | "user"
        bool isUser() const { return source == "user"; }
    };

    explicit AnalysisRepository(Database& db) : m_db(db) {}

    bool current(qint64 recordingId, Kind kind, Entry* out) const;
    // Stores an AI result as the new current one. If the user has written their own, that stays and *keptUser is set.
    bool saveAi(qint64 recordingId, Kind kind, const QString& text, const QString& model, bool* keptUser = nullptr,
                QString* error = nullptr);
    // The user's text (also empty = "no title": the card falls back to the file name). Becomes the current one.
    bool saveUser(qint64 recordingId, Kind kind, const QString& text, QString* error = nullptr);
    // The analysis found nothing to say (no speech): AI text from an earlier analysis must not stay on the card.
    // The user's own text is never touched.
    bool clearAi(qint64 recordingId, Kind kind);
    // Drop the user's text so the AI result of the last run is current again (if any).
    bool revertToAi(qint64 recordingId, Kind kind);

    static const char* kindName(Kind k) { return k == Title ? "title" : "summary"; }

private:
    bool insert(qint64 recordingId, Kind kind, const QString& text, const QString& source, const QString& model, QString* error);
    Database& m_db;
};

} // namespace ake
