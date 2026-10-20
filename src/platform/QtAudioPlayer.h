#pragma once
#include <memory>

#include "core/IAudioPlayer.h"

class QMediaPlayer;
class QAudioOutput;

namespace ake {

// QtMultimedia backend (GStreamer on Linux). Only this file pair knows about QMediaPlayer.
class QtAudioPlayer : public IAudioPlayer {
    Q_OBJECT
public:
    explicit QtAudioPlayer(QObject* parent = nullptr);
    ~QtAudioPlayer() override;

    void load(const QString& path) override;
    void play() override;
    void pause() override;
    void stop() override;
    void seek(qint64 ms) override;
    // Off by default: with Qt 6.2's GStreamer backend a speed change either does nothing (set before playback) or stalls
    // the pipeline for good (set during playback; measured on Ubuntu 22.04, no `scaletempo` plugin). AKE_ENABLE_RATE=1
    // switches it on for experiments.
    bool supportsRate() const override { return m_rateEnabled; }
    void setRate(double rate) override;
    void setVolume(double volume) override;

    State state() const override;
    qint64 position() const override;
    qint64 duration() const override;
    QString source() const override { return m_source; }
    double rate() const override { return m_rate; }
    double volume() const override { return m_volume; }

private:
    void onStatus(int status);
    void applyPendingSeek();
    QMediaPlayer* m_player;
    QAudioOutput* m_output;
    QString m_source;
    double m_rate = 1.0, m_volume = 1.0;
    bool m_rateEnabled = false;
    qint64 m_pendingSeek = -1;       // requested before the media was ready
    bool m_ready = false;
    bool m_playWhenReady = false;
};

} // namespace ake
