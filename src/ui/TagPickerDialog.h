#pragma once
#include <QDialog>

#include "core/RecordingFilter.h"

class QLineEdit;
class QListWidget;

namespace ake {

// "すべてのタグ…": filterable checklist of every tag. Full tag management is v0.5 (S8).
class TagPickerDialog : public QDialog {
    Q_OBJECT
public:
    TagPickerDialog(const QList<SidebarData::Tag>& tags, const QList<qint64>& selected, QWidget* parent = nullptr);
    QList<qint64> selectedTagIds() const;

    QLineEdit* filterEdit() const { return m_filter; }
    QListWidget* list() const { return m_list; }

private:
    void applyFilter(const QString& text);
    QLineEdit* m_filter;
    QListWidget* m_list;
};

} // namespace ake
