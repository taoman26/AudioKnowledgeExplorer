#pragma once
#include <QDialog>
#include <functional>

#include "core/Library.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

namespace ake {

class Database;
class ScanService;

// Folder management (design.md section 11, mock-up 05): the watched folders with their state, add / remove / rescan,
// recursion and exclude patterns. Changes go straight to the library and trigger a rescan.
class FolderDialog : public QDialog {
    Q_OBJECT
public:
    using Chooser = std::function<QString()>;              // returns a directory, or "" when cancelled
    using Confirm = std::function<bool(const QString& title, const QString& text)>;

    FolderDialog(Database& db, ScanService& scan, QWidget* parent = nullptr);

    // Injectable for tests; the defaults are QFileDialog / QMessageBox.
    void setChooser(Chooser c) { m_chooser = std::move(c); }
    void setConfirm(Confirm c) { m_confirm = std::move(c); }

    // --- for tests
    QTreeWidget* tree() const { return m_tree; }
    QPushButton* addButton() const { return m_add; }
    QPushButton* removeButton() const { return m_remove; }
    QPushButton* rescanButton() const { return m_rescan; }
    QLineEdit* excludeEdit() const { return m_exclude; }
    QCheckBox* recursiveBox() const { return m_recursive; }
    QString message() const;
    QString statusOf(int row) const;
    int rowForFolder(qint64 id) const;
    void selectFolder(qint64 id);
    void applyExclude();                  // what leaving the field / pressing Enter does

    static constexpr int kPathCol = 0, kCountCol = 1, kRecursiveCol = 2, kStateCol = 3;

signals:
    void libraryChanged();                // folders added/removed or a scan changed the recordings

private:
    void reload();
    void updateButtons();
    void addFolder();
    void removeFolder();
    void rescan();
    void onScanStarted(qint64 id);
    void onScanFinished(qint64 id, const ScanResult& r);
    void showMessage(const QString& text, bool error);
    qint64 currentId() const;

    Database& m_db;
    ScanService& m_scan;
    Chooser m_chooser;
    Confirm m_confirm;
    QTreeWidget* m_tree;
    QPushButton* m_add;
    QPushButton* m_remove;
    QPushButton* m_rescan;
    QLineEdit* m_exclude;
    QCheckBox* m_recursive;
    QLabel* m_message;
    QHash<qint64, QString> m_state;       // per folder: what the last scan said ("" = not scanned in this session)
};

} // namespace ake
