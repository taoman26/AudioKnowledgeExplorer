#include "ui/PlaybackController.h"

#include <QFileInfo>

#include "core/Database.h"
#include "core/RecordingDetail.h"
#include "core/TranscriptRepository.h"

namespace ake {

PlaybackController::PlaybackController(Database& db, IAudioPlayer& player, QObject* parent)
    : QObject(parent), m_db(db), m_player(player)
{
    connect(&m_player, &IAudioPlayer::stateChanged, this, &PlaybackController::stateChanged);
    connect(&m_player, &IAudioPlayer::durationChanged, this, &PlaybackController::durationChanged);
    connect(&m_player, &IAudioPlayer::positionChanged, this, [this](qint64 ms) {
        updateSentence(ms);
        emit positionChanged(ms);
    });
    connect(&m_player, &IAudioPlayer::mediaEnded, this, [this] { emit stateChanged(State::Stopped); });
    connect(&m_player, &IAudioPlayer::errorOccurred, this, [this](const QString& message) {
        m_loadedOk = false;                       // the next play() reloads from scratch
        emit errorOccurred(message);
    });
}

bool PlaybackController::play(qint64 id, qint64 startMs)
{
    if (id == m_id && m_loadedOk) {               // same recording: just move and go
        m_player.seek(startMs);
        m_player.play();
        return true;
    }
    const RecordingDetail d = RecordingDetail::load(m_db, id);
    if (!d.valid()) { emit errorOccurred(tr("録音が見つかりません")); return false; }
    if (!QFileInfo::exists(d.absPath)) {
        emit errorOccurred(tr("ファイルが見つかりません: %1").arg(d.absPath));
        return false;
    }
    if (m_id != 0) m_player.stop();
    const bool changed = id != m_id;
    m_id = id;
    m_loadedOk = true;
    m_title = d.title.exists ? d.title.content : QFileInfo(d.relPath).fileName();
    m_starts.clear(); m_texts.clear(); m_sentence.clear(); m_sentenceRow = -1;
    for (const auto& s : TranscriptRepository(m_db).loadSegments(id)) { m_starts << s.startMs; m_texts << s.effective(); }
    if (changed) emit recordingChanged(id);
    m_player.load(d.absPath);
    m_player.seek(startMs);
    m_player.play();
    updateSentence(startMs);
    return true;
}

void PlaybackController::toggle()
{
    if (m_id == 0) return;
    if (m_player.state() == State::Playing) { m_player.pause(); return; }
    if (!m_loadedOk) { play(m_id, 0); return; }
    if (m_player.state() == State::Stopped) m_player.seek(0);   // after the end: from the top
    m_player.play();
}

void PlaybackController::pause() { m_player.pause(); }

void PlaybackController::stop()
{
    if (m_id == 0) return;
    m_player.stop();
    m_id = 0;
    m_loadedOk = false;
    m_title.clear(); m_sentence.clear(); m_sentenceRow = -1;
    m_starts.clear(); m_texts.clear();
    emit recordingChanged(0);
}

void PlaybackController::seekTo(qint64 ms)
{
    if (m_id == 0) return;
    const qint64 dur = m_player.duration();
    ms = qMax<qint64>(0, ms);
    if (dur > 0) ms = qMin(ms, dur);
    m_player.seek(ms);
    updateSentence(ms);
}

void PlaybackController::skip(qint64 deltaMs) { seekTo(m_player.position() + deltaMs); }
void PlaybackController::setRate(double r) { m_player.setRate(r); }
void PlaybackController::setVolume(double v) { m_player.setVolume(v); }

void PlaybackController::updateSentence(qint64 ms)
{
    int row = -1;
    if (!m_starts.isEmpty()) {                     // last sentence starting at or before `ms`
        int lo = 0, hi = int(m_starts.size()) - 1;
        row = 0;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            if (m_starts[mid] <= ms) { row = mid; lo = mid + 1; } else hi = mid - 1;
        }
    }
    if (row == m_sentenceRow) return;
    m_sentenceRow = row;
    m_sentence = row >= 0 ? m_texts[row] : QString();
    emit sentenceChanged(m_sentence, row >= 0 ? m_starts[row] : 0);
}

} // namespace ake
