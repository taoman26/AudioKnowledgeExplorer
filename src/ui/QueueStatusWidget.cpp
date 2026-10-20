#include "ui/QueueStatusWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>

namespace ake {

QueueStatusWidget::QueueStatusWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("queueStatus");
    auto* l = new QHBoxLayout(this);
    l->setContentsMargins(0, 0, 8, 0);
    l->setSpacing(8);
    m_label = new QLabel;
    m_bar = new QProgressBar;
    m_bar->setRange(0, 100);
    m_bar->setTextVisible(false);
    m_bar->setFixedSize(130, 10);
    m_percent = new QLabel;
    m_queued = new QLabel;
    l->addWidget(m_label);
    l->addWidget(m_bar);
    l->addWidget(m_percent);
    l->addWidget(m_queued);
    setIdle();
}

void QueueStatusWidget::setRunning(const QString& name, const QString& stageText, double overall)
{
    m_running = true;
    m_label->setText(QStringLiteral("%1: %2").arg(stageText, name));
    const int pct = qBound(0, int(overall * 100 + 0.5), 100);
    m_bar->setValue(pct);
    m_percent->setText(QStringLiteral("%1%").arg(pct));
    refreshVisibility();
}

void QueueStatusWidget::setQueued(int queued, bool paused)
{
    m_queuedCount = queued;
    m_paused = paused;
    refreshVisibility();
}

void QueueStatusWidget::setIdle()
{
    m_running = false;
    m_queuedCount = 0;
    m_paused = false;
    m_label->clear();
    m_bar->setValue(0);
    m_percent->clear();
    refreshVisibility();
}

void QueueStatusWidget::refreshVisibility()
{
    m_label->setVisible(m_running);
    m_bar->setVisible(m_running);
    m_percent->setVisible(m_running);
    QStringList parts;
    if (m_queuedCount > 0) parts << tr("待機 %1").arg(m_queuedCount);
    if (m_paused) parts << tr("一時停止中");
    m_queued->setText(parts.join(" ・ "));
    m_queued->setVisible(!parts.isEmpty());
    setVisible(m_running || !parts.isEmpty());
}

QString QueueStatusWidget::text() const
{
    QStringList t;
    if (m_running) t << m_label->text() << m_percent->text();
    if (!m_queued->isHidden()) t << m_queued->text();
    return t.join("  ");
}

int QueueStatusWidget::percent() const { return m_bar->value(); }

} // namespace ake
