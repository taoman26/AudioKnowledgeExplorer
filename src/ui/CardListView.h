#pragma once
#include <QListView>

#include "ui/CardDelegate.h"
#include "ui/RowDelegate.h"

namespace ake {

class RowHeader;

// The recordings, as a wrapping grid of knowledge cards or as dense rows. Emits actions for the in-card/row buttons and
// the selected recording id; both modes show the same model.
class CardListView : public QListView {
    Q_OBJECT
public:
    enum class Mode { Cards, Rows };

    explicit CardListView(QWidget* parent = nullptr);
    CardDelegate* cardDelegate() const { return m_delegate; }
    RowDelegate* rowDelegate() const { return m_rowDelegate; }
    void setTheme(const Theme& t);               // both delegates + the column header

    Mode mode() const { return m_mode; }
    void setMode(Mode m);                        // keeps the selection and scrolls it into view
    QRect actionRect(const QModelIndex& idx, CardAction a) const;   // where a card's / row's button is (viewport coordinates)
    QWidget* headerWidget() const;               // the column labels of the row mode (hidden in card mode)

signals:
    void cardActionRequested(qint64 recordingId, ake::CardAction action);
    void currentRecordingChanged(qint64 recordingId); // 0 when selection cleared
    void modeChanged(ake::CardListView::Mode mode);

protected:
    void mouseReleaseEvent(QMouseEvent* e) override;
    void currentChanged(const QModelIndex& cur, const QModelIndex& prev) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void layoutHeader();
    CardDelegate* m_delegate;
    RowDelegate* m_rowDelegate;
    RowHeader* m_header;
    Mode m_mode = Mode::Cards;
};

} // namespace ake
