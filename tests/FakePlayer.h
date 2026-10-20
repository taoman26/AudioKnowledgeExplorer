#pragma once
// A player that "plays" without audio: time only moves when the test calls advance(). Lets the controller and the UI be
// tested deterministically; the real QtMultimedia backend has its own test (tst_qtplayer).
#include <QFileInfo>
#include <QTimer>
#include <algorithm>

#include "core/IAudioPlayer.h"

class FakePlayer : public ake::IAudioPlayer {   // no Q_OBJECT: it only emits the base class's signals
public:
    using State = ake::IAudioPlayer::State;
    explicit FakePlayer(QObject* parent = nullptr) : ake::IAudioPlayer(parent) {}

    qint64 fakeDuration = 20000;
    int loads = 0, plays = 0, pauses = 0, stops = 0;
    QList<qint64> seeks;

    void load(const QString& path) override
    {
        ++loads;
        m_source = path;
        m_pos = 0;
        setState(State::Stopped);
        if (path.contains("corrupt")) {              // exists, but cannot be decoded
            QTimer::singleShot(0, this, [this] { emit errorOccurred("ストリームの種類を判別できません"); });
            return;
        }
        m_dur = fakeDuration;
        emit durationChanged(m_dur);
    }
    void play() override { ++plays; if (!m_source.isEmpty()) setState(State::Playing); }
    void pause() override { ++pauses; if (m_state == State::Playing) setState(State::Paused); }
    void stop() override { ++stops; m_pos = 0; setState(State::Stopped); emit positionChanged(0); }
    void seek(qint64 ms) override { seeks << ms; m_pos = std::clamp<qint64>(ms, 0, m_dur > 0 ? m_dur : ms); emit positionChanged(m_pos); }
    bool rateSupported = true;
    bool supportsRate() const override { return rateSupported; }
    void setRate(double r) override { if (rateSupported) m_rate = r; }
    void setVolume(double v) override { m_volume = v; }

    State state() const override { return m_state; }
    qint64 position() const override { return m_pos; }
    qint64 duration() const override { return m_dur; }
    QString source() const override { return m_source; }
    double rate() const override { return m_rate; }
    double volume() const override { return m_volume; }

    // the clock: moves time forward while playing, ends the media at the end
    void advance(qint64 ms)
    {
        if (m_state != State::Playing) return;
        m_pos += ms;
        if (m_pos >= m_dur) { m_pos = 0; setState(State::Stopped); emit positionChanged(0); emit mediaEnded(); return; }
        emit positionChanged(m_pos);
    }

private:
    void setState(State s) { if (s == m_state) return; m_state = s; emit stateChanged(s); }
    State m_state = State::Stopped;
    QString m_source;
    qint64 m_pos = 0, m_dur = 0;
    double m_rate = 1.0, m_volume = 1.0;
};
