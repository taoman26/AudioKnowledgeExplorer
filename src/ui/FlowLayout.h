#pragma once
#include <QLayout>
#include <QList>

namespace ake {

// Left-to-right layout that wraps onto new rows (chips). Height depends on the width it is given.
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent = nullptr, int hSpacing = 6, int vSpacing = 6) : QLayout(parent), m_h(hSpacing), m_v(vSpacing) {}
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override { m_items.append(item); }
    int count() const override { return int(m_items.size()); }
    QLayoutItem* itemAt(int i) const override { return i >= 0 && i < m_items.size() ? m_items[i] : nullptr; }
    QLayoutItem* takeAt(int i) override { return i >= 0 && i < m_items.size() ? m_items.takeAt(i) : nullptr; }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return doLayout(QRect(0, 0, w, 0), true); }
    void setGeometry(const QRect& r) override { QLayout::setGeometry(r); doLayout(r, false); }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override;

private:
    int doLayout(const QRect& rect, bool testOnly) const;
    QList<QLayoutItem*> m_items;
    int m_h, m_v;
};

} // namespace ake
