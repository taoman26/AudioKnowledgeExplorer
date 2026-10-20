#include "ui/CardListView.h"

#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>

#include "ui/RecordingListModel.h"

namespace ake {

// Column labels above the rows; the geometry comes from RowDelegate::columns() so they line up with the rows.
class RowHeader : public QWidget {
public:
    explicit RowHeader(QWidget* parent) : QWidget(parent), m_theme(Theme::light()) { setObjectName("rowHeader"); }
    void setTheme(const Theme& t) { m_theme = t; update(); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), m_theme.panel);
        p.setPen(m_theme.line);
        p.drawLine(0, height() - 1, width(), height() - 1);
        QFont f = font();
        f.setPixelSize(11);
        f.setBold(true);
        p.setFont(f);
        p.setPen(m_theme.faint);
        const auto c = RowDelegate::columns(width());
        auto label = [&](int x, int w, const QString& s, Qt::Alignment al = Qt::AlignLeft) {
            p.drawText(QRect(x, 0, w, height()), al | Qt::AlignVCenter, s);
        };
        label(c.textX, c.textW, tr("タイトル / 要約"));
        if (c.showTags) label(c.tagsX, c.tagsW, tr("タグ"));
        label(c.dateX, c.dateW, tr("作成日時"));
        label(c.durX, c.durW, tr("長さ"), Qt::AlignRight);
    }

private:
    Theme m_theme;
};

CardListView::CardListView(QWidget* parent)
    : QListView(parent), m_delegate(new CardDelegate(this)), m_rowDelegate(new RowDelegate(this)), m_header(new RowHeader(this))
{
    setObjectName("cardList");
    setItemDelegate(m_delegate);
    setViewMode(QListView::IconMode);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setUniformItemSizes(true);
    setSpacing(14);
    setMouseTracking(true);
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    verticalScrollBar()->setSingleStep(40);
    viewport()->setAutoFillBackground(false);
    m_header->hide();
}

void CardListView::setTheme(const Theme& t)
{
    m_delegate->setTheme(t);
    m_rowDelegate->setTheme(t);
    m_header->setTheme(t);
    viewport()->update();
}

QWidget* CardListView::headerWidget() const { return m_header; }

void CardListView::setMode(Mode m)
{
    if (m == m_mode) return;
    const QModelIndex keep = currentIndex();
    m_mode = m;
    if (m == Mode::Rows) {
        setViewMode(QListView::ListMode);
        setFlow(QListView::TopToBottom);
        setWrapping(false);
        setSpacing(0);
        setUniformItemSizes(true);
        setResizeMode(QListView::Adjust);
        setMovement(QListView::Static);
        m_rowDelegate->setWidth(viewport()->width());
        setItemDelegate(m_rowDelegate);
        setViewportMargins(0, RowDelegate::kHeaderHeight, 0, 0);
        m_header->show();
        verticalScrollBar()->setSingleStep(31);
    } else {
        setViewMode(QListView::IconMode);
        setResizeMode(QListView::Adjust);
        setMovement(QListView::Static);
        setUniformItemSizes(true);
        setSpacing(14);
        setItemDelegate(m_delegate);
        setViewportMargins(0, 0, 0, 0);
        m_header->hide();
        verticalScrollBar()->setSingleStep(40);
    }
    layoutHeader();
    // after the viewport margin changed the viewport width is final: lay the rows out for it
    if (m == Mode::Rows) m_rowDelegate->setWidth(viewport()->width());
    doItemsLayout();
    if (keep.isValid()) { setCurrentIndex(keep); scrollTo(keep); }
    emit modeChanged(m);
}

void CardListView::layoutHeader()
{
    // the header spans the viewport (not the scroll bar) so that labels line up with the rows
    m_header->setGeometry(0, 0, viewport()->width(), RowDelegate::kHeaderHeight);
}

void CardListView::resizeEvent(QResizeEvent* e)
{
    QListView::resizeEvent(e);
    if (m_mode == Mode::Rows) {
        m_rowDelegate->setWidth(viewport()->width());
        layoutHeader();
        doItemsLayout();                        // the row width changed
    }
}

QRect CardListView::actionRect(const QModelIndex& idx, CardAction a) const
{
    return m_mode == Mode::Rows ? RowDelegate::actionButtonRect(visualRect(idx), a)
                                : CardDelegate::actionButtonRect(visualRect(idx), a);
}

void CardListView::mouseReleaseEvent(QMouseEvent* e)
{
    const QModelIndex idx = indexAt(e->pos());
    QListView::mouseReleaseEvent(e);
    if (!idx.isValid() || e->button() != Qt::LeftButton)
        return;
    CardAction a;
    if (!CardDelegate::actionFor(idx, &a))
        return;
    if (actionRect(idx, a).contains(e->pos()))
        emit cardActionRequested(idx.data(RecordingListModel::IdRole).toLongLong(), a);
}

void CardListView::currentChanged(const QModelIndex& cur, const QModelIndex& prev)
{
    QListView::currentChanged(cur, prev);
    emit currentRecordingChanged(cur.isValid() ? cur.data(RecordingListModel::IdRole).toLongLong() : 0);
}

} // namespace ake
