#pragma once
#include <QStyledItemDelegate>
#include <QTreeWidget>

#include "core/RecordingFilter.h"
#include "ui/Theme.h"

namespace ake {

// Library / folders / tags / period navigation (design.md section 5).
// Library and folder items choose the scope (exclusive); period is a toggle; tags toggle and combine with AND.
class SidebarWidget : public QTreeWidget {
    Q_OBJECT
public:
    enum Kind { Header = 1, Library, Folder, Tag, PeriodItem, MoreTags };
    enum Role { KindRole = Qt::UserRole + 1, KeyRole, CountRole, ActiveRole, IconRole };
    static constexpr int kTopTags = 6;

    explicit SidebarWidget(QWidget* parent = nullptr);

    void setTheme(const Theme& t);
    // Replaces counts/items, keeping the current selection (dropping parts that no longer exist).
    void setData(const SidebarData& d);
    const RecordingFilter& filter() const { return m_filter; }
    void setFilter(const RecordingFilter& f); // also used by the "all tags" dialog
    const SidebarData& data() const { return m_data; }

    QTreeWidgetItem* findItem(Kind kind, qint64 key) const;
    void showFolderMenu(const QPoint& pos);          // the right-click menu of a folder row (also called by tests)
    static QString folderLabel(const QString& path);

signals:
    void filterChanged(const ake::RecordingFilter& filter);
    void allTagsRequested();
    void folderManageRequested(qint64 folderId);     // right-click on a folder
    void folderRemoveRequested(qint64 folderId);

private:
    void rebuild();
    void activate(QTreeWidgetItem* item);
    QTreeWidgetItem* add(Kind k, const QString& text, qint64 key, int count, const QString& icon);
    void addHeader(const QString& text);

    RecordingFilter m_filter;
    SidebarData m_data;
    class SidebarDelegate* m_delegate;
};

class SidebarDelegate : public QStyledItemDelegate {
public:
    explicit SidebarDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent), m_theme(Theme::light()) {}
    void setTheme(const Theme& t) { m_theme = t; }
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& idx) const override;

private:
    Theme m_theme;
};

} // namespace ake
