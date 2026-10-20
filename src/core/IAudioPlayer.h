#pragma once
#include <QObject>
#include <QString>

namespace ake {

// Playback of one audio file. The app only ever talks to this interface: Linux uses QtMultimedia, Haiku can plug in
// the Media Kit (BSoundPlayer) without touching anything above. All times are in milliseconds.
class IAudioPlayer : public QObject {
    Q_OBJECT
public:
    enum class State { Stopped, Playing, Paused };
    Q_ENUM(State)

    explicit IAudioPlayer(QObject* parent = nullptr) : QObject(parent) {}

    // Starts loading `path` (asynchronous). Failure is reported through errorOccurred, never by throwing.
    virtual void load(const QString& path) = 0;
    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void stop() = 0;                     // back to the start; the file stays loaded
    virtual void seek(qint64 ms) = 0;            // allowed before the media is ready (applied as soon as it is)
    // Playback speed (1.0 = normal). Only meaningful when supportsRate(): a backend that cannot change speed reliably
    // says so, ignores setRate(), and keeps reporting 1.0 (the UI then hides the speed control).
    virtual bool supportsRate() const { return false; }
    virtual void setRate(double rate) = 0;
    virtual void setVolume(double volume) = 0;   // 0..1

    virtual State state() const = 0;
    virtual qint64 position() const = 0;
    virtual qint64 duration() const = 0;         // 0 until known
    virtual QString source() const = 0;
    virtual double rate() const = 0;
    virtual double volume() const = 0;

signals:
    void stateChanged(ake::IAudioPlayer::State state);
    void positionChanged(qint64 ms);
    void durationChanged(qint64 ms);
    void errorOccurred(const QString& message);  // the player is left stopped
    void mediaEnded();                           // reached the end (state becomes Stopped, position 0)
};

} // namespace ake
