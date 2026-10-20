#pragma once
#include <QDateTime>
#include <QList>
#include <QString>

namespace ake {

class Database;

// Everything the detail pane shows about one recording, read in one go.
struct RecordingDetail {
    struct Text { QString content; bool isUser = false; bool exists = false; bool aiAvailable = false; };
    struct Kw { qint64 id = 0; QString name; double score = 0; bool isUser = false; };

    qint64 id = 0;
    QString relPath, absPath, status, error, format;
    QDateTime recordedAt;
    qint64 durationMs = 0;
    bool favorite = false;
    QString language, model;       // of the current transcript (empty when there is none)
    int segmentCount = 0;
    Text title, summary;           // title.exists == false: the card shows the file name
    QList<Kw> keywords;            // best first (user keywords before AI ones)
    QString notes;

    bool valid() const { return id != 0; }
    static RecordingDetail load(Database& db, qint64 id); // valid() == false if the recording does not exist
};

} // namespace ake
