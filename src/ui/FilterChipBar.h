#pragma once
#include <QList>
#include <QString>
#include <QWidget>

class QPushButton;

namespace ake {

class FlowLayout;

// The conditions that narrow the list right now (search words, folder/scope, tags, period), each with a ✕ that removes just
// that one, and "すべて解除" when there are several. Hidden when nothing narrows. Design: "検索から絞り込む流れ".
class FilterChipBar : public QWidget {
    Q_OBJECT
public:
    enum Kind { Search = 1, Scope, Tag, Period };
    struct Chip {
        Kind kind;
        qint64 key;       // tag id; unused for the others
        QString text;
    };

    explicit FilterChipBar(QWidget* parent = nullptr);

    void setChips(const QList<Chip>& chips);
    const QList<Chip>& chips() const { return m_chips; }
    QList<QPushButton*> chipButtons() const;     // for tests
    QPushButton* clearAllButton() const { return m_clearAll; }

signals:
    void removeRequested(int kind, qint64 key);
    void clearAllRequested();

private:
    QList<Chip> m_chips;
    FlowLayout* m_flow;
    QPushButton* m_clearAll;
};

} // namespace ake
