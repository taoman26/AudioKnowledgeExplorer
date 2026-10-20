#include "ui/MiniPlayer.h"

#include <QActionGroup>
#include <climits>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/PlaybackController.h"
#include "ui/TranscriptModel.h"

namespace ake {

static QToolButton* button(const QString& text, const QString& tip, const char* name = "playerButton")
{
    auto* b = new QToolButton;
    b->setObjectName(name);
    b->setText(text);
    b->setToolTip(tip);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

QString MiniPlayer::formatClock(qint64 ms)
{
    const qint64 s = qMax<qint64>(0, ms) / 1000;
    return s >= 3600 ? QStringLiteral("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'))
                     : QStringLiteral("%1:%2").arg(s / 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
}

MiniPlayer::MiniPlayer(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("miniPlayer");
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(76);
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(20, 8, 20, 8);
    root->setSpacing(16);

    auto* left = new QVBoxLayout;
    m_title = new QLabel;
    m_title->setObjectName("playerTitle");
    m_sentence = new QLabel;
    m_sentence->setObjectName("detailSub");
    m_error = new QLabel;
    m_error->setObjectName("playerError");
    m_error->hide();
    left->addWidget(m_title);
    left->addWidget(m_sentence);
    left->addWidget(m_error);
    auto* leftBox = new QWidget;
    leftBox->setLayout(left);
    leftBox->setMinimumWidth(180);
    leftBox->setMaximumWidth(380);
    root->addWidget(leftBox);

    auto* mid = new QVBoxLayout;
    auto* controls = new QHBoxLayout;
    controls->setSpacing(10);
    m_back = button(tr("−5秒"), tr("5秒戻る"));
    m_play = button("▶", tr("再生 / 一時停止 (Space)"), "playerPlay");
    m_fwd = button(tr("+5秒"), tr("5秒進む"));
    controls->addStretch();
    controls->addWidget(m_back);
    controls->addWidget(m_play);
    controls->addWidget(m_fwd);
    controls->addStretch();
    auto* seekRow = new QHBoxLayout;
    m_time = new QLabel("0:00 / 0:00");
    m_time->setObjectName("detailSub");
    m_time->setMinimumWidth(90);
    m_seek = new QSlider(Qt::Horizontal);
    m_seek->setObjectName("seekSlider");
    m_seek->setRange(0, 0);
    seekRow->addWidget(m_seek, 1);
    seekRow->addWidget(m_time);
    mid->addLayout(controls);
    mid->addLayout(seekRow);
    root->addLayout(mid, 1);

    auto* right = new QHBoxLayout;
    right->setSpacing(10);
    m_speed = button("1.0x ▾", tr("再生速度"), "playerSpeed");
    m_speed->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(m_speed);
    auto* group = new QActionGroup(menu);
    for (double r : { 0.75, 1.0, 1.25, 1.5, 2.0 }) {
        QAction* a = menu->addAction(QStringLiteral("%1x").arg(r, 0, 'f', r == 1.0 || r == 2.0 ? 1 : 2));
        a->setCheckable(true);
        a->setChecked(r == 1.0);
        a->setData(r);
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, r] { if (m_ctl) m_ctl->setRate(r); refresh(); });
    }
    m_speed->setMenu(menu);
    m_volume = new QSlider(Qt::Horizontal);
    m_volume->setObjectName("volumeSlider");
    m_volume->setRange(0, 100);
    m_volume->setValue(100);
    m_volume->setFixedWidth(90);
    m_volume->setToolTip(tr("音量"));
    m_close = button("✕", tr("プレーヤーを閉じる"), "playerClose");
    right->addWidget(m_speed);
    right->addWidget(m_volume);
    right->addWidget(m_close);
    root->addLayout(right);

    connect(m_play, &QToolButton::clicked, this, [this] { if (m_ctl) m_ctl->toggle(); });
    connect(m_back, &QToolButton::clicked, this, [this] { if (m_ctl) m_ctl->skip(-PlaybackController::kSkipMs); });
    connect(m_fwd, &QToolButton::clicked, this, [this] { if (m_ctl) m_ctl->skip(PlaybackController::kSkipMs); });
    connect(m_close, &QToolButton::clicked, this, [this] { if (m_ctl) m_ctl->stop(); });
    connect(m_volume, &QSlider::valueChanged, this, [this](int v) { if (m_ctl) m_ctl->setVolume(v / 100.0); });
    // dragging: show the target time but only seek on release (seeking on every move would stutter the audio)
    connect(m_seek, &QSlider::sliderPressed, this, [this] { m_dragging = true; });
    connect(m_seek, &QSlider::sliderMoved, this, [this](int v) { refreshTime(v); });
    connect(m_seek, &QSlider::sliderReleased, this, [this] {
        m_dragging = false;
        if (m_ctl) m_ctl->seekTo(m_seek->value());
    });
    hide();
}

void MiniPlayer::setController(PlaybackController* c)
{
    m_ctl = c;
    m_speed->setVisible(c && c->supportsRate());     // a backend that cannot change speed: no speed control at all
    if (!c) return;
    connect(c, &PlaybackController::recordingChanged, this, [this](qint64 id) {
        m_error->hide();
        setVisible(id != 0);
        refresh();
    });
    connect(c, &PlaybackController::stateChanged, this, [this] { refresh(); });
    connect(c, &PlaybackController::durationChanged, this, [this](qint64 d) { m_seek->setRange(0, int(qMin<qint64>(d, INT_MAX))); refreshTime(m_ctl->position()); });
    connect(c, &PlaybackController::positionChanged, this, [this](qint64 ms) {
        if (m_dragging) return;                         // the user is moving the knob
        QSignalBlocker block(m_seek);
        m_seek->setValue(int(qMin<qint64>(ms, INT_MAX)));
        refreshTime(ms);
    });
    connect(c, &PlaybackController::sentenceChanged, this, [this](const QString& text, qint64 startMs) {
        m_fullSentence = text.isEmpty() ? QString() : QStringLiteral("%1 ・ %2").arg(TranscriptModel::formatTime(startMs), text);
        m_sentence->setText(m_sentence->fontMetrics().elidedText(m_fullSentence, Qt::ElideRight, qMax(120, m_sentence->width())));
    });
    connect(c, &PlaybackController::errorOccurred, this, [this](const QString& m) { showError(m); });
}

void MiniPlayer::refresh()
{
    if (!m_ctl) return;
    m_title->setText(m_ctl->title());
    m_play->setText(m_ctl->isPlaying() ? "❚❚" : "▶");
    m_speed->setText(QStringLiteral("%1x ▾").arg(m_ctl->rate(), 0, 'f', m_ctl->rate() == 1.0 || m_ctl->rate() == 2.0 ? 1 : 2));
    for (QAction* a : m_speed->menu()->actions()) a->setChecked(qFuzzyCompare(a->data().toDouble(), m_ctl->rate()));
    refreshTime(m_ctl->position());
}

void MiniPlayer::refreshTime(qint64 position)
{
    m_time->setText(QStringLiteral("%1 / %2").arg(formatClock(position), formatClock(m_ctl ? m_ctl->duration() : 0)));
}

void MiniPlayer::showError(const QString& message)
{
    m_error->setText(m_error->fontMetrics().elidedText(tr("再生できません: %1").arg(message), Qt::ElideRight, qMax(160, m_error->width())));
    m_error->setToolTip(message);
    m_error->show();
    m_sentence->hide();
    QTimer::singleShot(7000, this, [this] { m_error->hide(); m_sentence->show(); });
}

void MiniPlayer::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    m_sentence->setText(m_sentence->fontMetrics().elidedText(m_fullSentence, Qt::ElideRight, qMax(120, m_sentence->width())));
}

QString MiniPlayer::titleText() const { return m_title->text(); }
QString MiniPlayer::sentenceText() const { return m_fullSentence; }
QString MiniPlayer::timeText() const { return m_time->text(); }
QString MiniPlayer::errorText() const { return m_error->isHidden() ? QString() : m_error->text(); }

} // namespace ake
