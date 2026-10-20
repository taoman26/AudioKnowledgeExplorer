#pragma once
#include <QObject>
#include <QString>
#include <QVector>

#include "core/IAudioPlayer.h"

namespace ake {

class Database;

// Ties the audio player to the library: which recording is loaded, where it is, which sentence is being spoken.
// UI code talks to this, never to the player directly.
class PlaybackController : public QObject {
    Q_OBJECT
public:
    using State = IAudioPlayer::State;
    PlaybackController(Database& db, IAudioPlayer& player, QObject* parent = nullptr);

    qint64 recordingId() const { return m_id; }          // 0 = nothing loaded
    QString title() const { return m_title; }
    State state() const { return m_player.state(); }
    bool isPlaying() const { return m_player.state() == State::Playing; }
    qint64 position() const { return m_player.position(); }
    qint64 duration() const { return m_player.duration(); }
    QString sentence() const { return m_sentence; }       // the sentence at the current position ("" if none)
    double rate() const { return m_player.rate(); }
    bool supportsRate() const { return m_player.supportsRate(); }
    double volume() const { return m_player.volume(); }

    // Starts `recordingId` at `startMs`. The same recording that is already loaded is only seeked (no reload).
    // Returns false when it cannot be played (file missing, unknown id); errorOccurred() carries the reason and the
    // currently loaded recording is left alone.
    bool play(qint64 recordingId, qint64 startMs = 0);
    void toggle();                    // play <-> pause (from the start again after the end)
    void pause();
    void stop();                      // unload: recordingId() becomes 0
    void seekTo(qint64 ms);           // clamped to [0, duration]
    void skip(qint64 deltaMs);        // relative seek
    void setRate(double r);
    void setVolume(double v);

    static constexpr qint64 kSkipMs = 5000;

signals:
    void recordingChanged(qint64 recordingId);            // 0 when unloaded
    void stateChanged(ake::IAudioPlayer::State state);
    void positionChanged(qint64 ms);
    void durationChanged(qint64 ms);
    void sentenceChanged(const QString& text, qint64 startMs);
    void errorOccurred(const QString& message);

private:
    void updateSentence(qint64 ms);
    Database& m_db;
    IAudioPlayer& m_player;
    qint64 m_id = 0;
    bool m_loadedOk = false;
    QString m_title, m_sentence;
    QVector<qint64> m_starts;
    QVector<QString> m_texts;
    int m_sentenceRow = -1;
};

} // namespace ake
