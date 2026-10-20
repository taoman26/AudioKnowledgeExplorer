#pragma once
#include <QAbstractListModel>
#include <QRegularExpression>
#include <QVector>

#include "core/TranscriptRepository.h"

namespace ake {

class Database;

// One row per segment of the current transcript.
class TranscriptModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { StartMsRole = Qt::UserRole + 1, EndMsRole, OriginalRole, EditedRole, SegmentIdRole, HighlightsRole };
    struct Hit { int row; int start; int length; };

    explicit TranscriptModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& p = {}) const override { return p.isValid() ? 0 : int(m_rows.size()); }
    QVariant data(const QModelIndex& index, int role) const override;

    void load(Database& db, qint64 recordingId);
    qint64 recordingId() const { return m_recording; }
    void setSegmentText(int row, const QString& effective, bool edited); // after the editor saved it

    // Search inside the transcript: every term, case-insensitive, full-width/half-width folded (offsets stay valid).
    void setSearchTerms(const QStringList& terms);
    const QList<Hit>& hits() const { return m_hits; }
    void setCurrentHit(int index);          // -1 = none
    int currentHit() const { return m_currentHit; }
    QVector<int> highlightsOf(int row) const; // flat [start, length, ...] into the effective text
    int currentHitStartIn(int row) const;     // start offset of the current hit if it lies in this row, else -1

    int rowForTime(qint64 ms) const;        // the segment playing at `ms` (last one starting at or before it)
signals:
    void highlightsChanged();           // search hits / current hit changed: repaint only (row sizes are unaffected)

public:
    QString text(int row) const { return m_rows.value(row).effective(); }
    qint64 startMs(int row) const { return m_rows.value(row).startMs; }
    qint64 segmentId(int row) const { return m_rows.value(row).id; }
    bool isEdited(int row) const { return m_rows.value(row).edited(); }

    // Plain text of the whole transcript, one segment per line, optionally prefixed with "[hh:mm:ss] ".
    QString fullText(bool withTimes) const;
    static QString formatTime(qint64 ms);

private:
    void rebuildHits();
    QList<TranscriptRepository::SegmentRow> m_rows;
    qint64 m_recording = 0;
    QList<QRegularExpression> m_regexes;
    QList<Hit> m_hits;
    int m_currentHit = -1;
};

} // namespace ake
