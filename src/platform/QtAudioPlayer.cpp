#include "platform/QtAudioPlayer.h"

#include <QAudioOutput>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QUrl>

namespace ake {

QtAudioPlayer::QtAudioPlayer(QObject* parent)
    : IAudioPlayer(parent), m_player(new QMediaPlayer(this)), m_output(new QAudioOutput(this))
{
    m_rateEnabled = qEnvironmentVariableIntValue("AKE_ENABLE_RATE") == 1;
    m_player->setAudioOutput(m_output);
    m_output->setVolume(float(m_volume));
    connect(m_player, &QMediaPlayer::positionChanged, this, &IAudioPlayer::positionChanged);
    connect(m_player, &QMediaPlayer::durationChanged, this, &IAudioPlayer::durationChanged);
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState s) {
        applyPendingSeek();
        if (s == QMediaPlayer::PlayingState && m_player->playbackRate() != m_rate) m_player->setPlaybackRate(m_rate);   // a new source resets it
        emit stateChanged(s == QMediaPlayer::PlayingState ? State::Playing : s == QMediaPlayer::PausedState ? State::Paused : State::Stopped);
    });
    connect(m_player, &QMediaPlayer::seekableChanged, this, [this](bool seekable) {
        applyPendingSeek();
        if (seekable && m_player->playbackRate() != m_rate) m_player->setPlaybackRate(m_rate);   // rate changes need a seekable pipeline too
    });
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus s) { onStatus(int(s)); });
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString& text) {
        m_ready = false;
        m_playWhenReady = false;
        emit errorOccurred(text.isEmpty() ? tr("再生できません") : text);
    });
}

QtAudioPlayer::~QtAudioPlayer() = default;

// The GStreamer backend ignores setPosition() while stopped, and right after play() the pipeline is not seekable yet
// (seekableChanged follows a moment later). So a seek requested early waits until playing/paused AND seekable.
void QtAudioPlayer::applyPendingSeek()
{
    if (m_pendingSeek < 0 || !m_ready || m_player->playbackState() == QMediaPlayer::StoppedState || !m_player->isSeekable()) return;
    const qint64 ms = m_pendingSeek;
    m_pendingSeek = -1;
    m_player->setPosition(ms);
}

void QtAudioPlayer::onStatus(int status)
{
    const auto s = static_cast<QMediaPlayer::MediaStatus>(status);
    if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia || s == QMediaPlayer::BufferingMedia) {
        if (!m_ready) {
            m_ready = true;
            m_player->setPlaybackRate(m_rate);                  // the backend forgets the rate when a new file is loaded
            if (m_playWhenReady) { m_playWhenReady = false; m_player->play(); }
        }
    } else if (s == QMediaPlayer::EndOfMedia) {
        m_player->stop();
        m_pendingSeek = 0;                      // the next play() starts from the beginning
        emit positionChanged(0);
        emit mediaEnded();
    } else if (s == QMediaPlayer::InvalidMedia) {
        m_ready = false;
    }
}

void QtAudioPlayer::load(const QString& path)
{
    m_player->stop();
    m_ready = false;
    m_pendingSeek = -1;
    m_playWhenReady = false;
    m_source = path;
    if (!QFileInfo::exists(path)) {            // QMediaPlayer would only say "no resource": give a useful message
        // queued: a caller that connects after load() (or before it starts its event loop) must still see the error
        QMetaObject::invokeMethod(this, [this, path] { emit errorOccurred(tr("ファイルが見つかりません: %1").arg(path)); }, Qt::QueuedConnection);
        return;
    }
    m_player->setSource(QUrl::fromLocalFile(path));
}

void QtAudioPlayer::play()
{
    if (m_source.isEmpty()) return;
    if (m_ready) m_player->play(); else m_playWhenReady = true;
}

void QtAudioPlayer::pause() { m_playWhenReady = false; m_player->pause(); }

void QtAudioPlayer::stop()
{
    m_playWhenReady = false;
    m_pendingSeek = -1;
    m_player->stop();
}

void QtAudioPlayer::seek(qint64 ms)
{
    ms = qMax<qint64>(0, ms);
    if (m_ready && m_player->playbackState() != QMediaPlayer::StoppedState && m_player->isSeekable()) {
        m_player->setPosition(ms);
    } else {                                   // not playing yet: remember it, and show it right away
        m_pendingSeek = ms;
        emit positionChanged(ms);
    }
}

void QtAudioPlayer::setRate(double r)
{
    if (!m_rateEnabled) return;                    // see supportsRate(): changing it can stall playback for good
    m_rate = qBound(0.25, r, 4.0);
    m_player->setPlaybackRate(m_rate);
}
void QtAudioPlayer::setVolume(double v) { m_volume = qBound(0.0, v, 1.0); m_output->setVolume(float(m_volume)); }

IAudioPlayer::State QtAudioPlayer::state() const
{
    switch (m_player->playbackState()) {
    case QMediaPlayer::PlayingState: return State::Playing;
    case QMediaPlayer::PausedState: return State::Paused;
    default: return State::Stopped;
    }
}

qint64 QtAudioPlayer::position() const { return m_pendingSeek >= 0 ? m_pendingSeek : m_player->position(); }
qint64 QtAudioPlayer::duration() const { return m_player->duration(); }

} // namespace ake
