#include "ui/TagPickerDialog.h"

#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

namespace ake {

TagPickerDialog::TagPickerDialog(const QList<SidebarData::Tag>& tags, const QList<qint64>& selected, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("すべてのタグ"));
    resize(360, 480);
    auto* v = new QVBoxLayout(this);
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(tr("タグを絞り込み"));
    m_filter->setClearButtonEnabled(true);
    v->addWidget(m_filter);
    m_list = new QListWidget;
    for (const auto& t : tags) {
        auto* it = new QListWidgetItem(QStringLiteral("%1  (%2)").arg(t.name).arg(t.count), m_list);
        it->setData(Qt::UserRole, t.id);
        it->setData(Qt::UserRole + 1, t.name);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(selected.contains(t.id) ? Qt::Checked : Qt::Unchecked);
    }
    v->addWidget(m_list, 1);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    v->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_filter, &QLineEdit::textChanged, this, &TagPickerDialog::applyFilter);
}

void TagPickerDialog::applyFilter(const QString& text)
{
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem* it = m_list->item(i);
        it->setHidden(!it->data(Qt::UserRole + 1).toString().contains(text, Qt::CaseInsensitive));
    }
}

QList<qint64> TagPickerDialog::selectedTagIds() const
{
    QList<qint64> ids;
    for (int i = 0; i < m_list->count(); ++i)
        if (m_list->item(i)->checkState() == Qt::Checked) ids << m_list->item(i)->data(Qt::UserRole).toLongLong();
    return ids; // list order = most used first, hidden items keep their state
}

} // namespace ake
