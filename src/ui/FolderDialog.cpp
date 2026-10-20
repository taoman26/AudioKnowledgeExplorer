#include "ui/FolderDialog.h"

#include <QCheckBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "core/Database.h"
#include "core/FolderScanner.h"
#include "core/ScanService.h"

namespace ake {

FolderDialog::FolderDialog(Database& db, ScanService& scan, QWidget* parent)
    : QDialog(parent), m_db(db), m_scan(scan)
{
    setWindowTitle(tr("フォルダ管理"));
    resize(860, 520);
    m_chooser = [this] {
        return QFileDialog::getExistingDirectory(this, tr("録音フォルダを選択"), QString(), QFileDialog::ShowDirsOnly);
    };
    m_confirm = [this](const QString& title, const QString& text) {
        return QMessageBox::question(this, title, text, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
    };

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(24, 20, 24, 20);
    v->setSpacing(10);
    auto* head = new QLabel(tr("監視フォルダ"));
    head->setObjectName("detailTitle");
    v->addWidget(head);

    m_tree = new QTreeWidget;
    m_tree->setObjectName("folderTree");
    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({ tr("フォルダ"), tr("録音"), tr("サブフォルダ"), tr("状態") });
    m_tree->setRootIsDecorated(false);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setAlternatingRowColors(false);
    m_tree->header()->setSectionResizeMode(kPathCol, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(kCountCol, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(kRecursiveCol, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(kStateCol, QHeaderView::ResizeToContents);
    v->addWidget(m_tree, 1);

    auto* opts = new QHBoxLayout;
    m_recursive = new QCheckBox(tr("サブフォルダも対象にする"));
    opts->addWidget(m_recursive);
    opts->addStretch();
    v->addLayout(opts);
    auto* exRow = new QHBoxLayout;
    auto* exLabel = new QLabel(tr("除外パターン"));
    exLabel->setObjectName("detailSection");
    m_exclude = new QLineEdit;
    m_exclude->setPlaceholderText(tr("例: *.tmp, .Trash*, *_backup.*  （カンマ区切り。ファイル名・フォルダ名に一致）"));
    exRow->addWidget(exLabel);
    exRow->addWidget(m_exclude, 1);
    v->addLayout(exRow);
    auto* formats = new QLabel(tr("対応形式:  %1").arg(FolderScanner::supportedExtensions().join("   ")));
    formats->setObjectName("detailSub");
    v->addWidget(formats);
    m_message = new QLabel;
    m_message->setObjectName("folderMessage");
    m_message->setWordWrap(true);
    v->addWidget(m_message);

    auto* buttons = new QHBoxLayout;
    m_add = new QPushButton(tr("＋ 追加…"));
    m_add->setObjectName("primary");
    m_remove = new QPushButton(tr("削除"));
    m_rescan = new QPushButton(tr("再走査"));
    auto* close = new QPushButton(tr("閉じる"));
    buttons->addWidget(m_add);
    buttons->addWidget(m_remove);
    buttons->addWidget(m_rescan);
    buttons->addStretch();
    buttons->addWidget(close);
    v->addLayout(buttons);

    connect(m_add, &QPushButton::clicked, this, [this] { addFolder(); });
    connect(m_remove, &QPushButton::clicked, this, [this] { removeFolder(); });
    connect(m_rescan, &QPushButton::clicked, this, [this] { rescan(); });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this] { updateButtons(); });
    connect(m_recursive, &QCheckBox::clicked, this, [this](bool on) {
        Library lib(m_db);
        const qint64 id = currentId();
        if (!id) return;
        QString ex;
        for (const FolderInfo& f : lib.folders()) if (f.id == id) ex = f.excludeGlob;
        if (lib.setFolderOptions(id, on, ex)) { reload(); selectFolder(id); m_scan.scanFolder(id); }
    });
    connect(m_exclude, &QLineEdit::editingFinished, this, [this] { applyExclude(); });
    connect(&m_scan, &ScanService::started, this, [this](qint64 id) { onScanStarted(id); });
    connect(&m_scan, &ScanService::finished, this, [this](qint64 id, const ScanResult& r) { onScanFinished(id, r); });
    reload();
}

qint64 FolderDialog::currentId() const
{
    const QTreeWidgetItem* it = m_tree->currentItem();
    return it ? it->data(kPathCol, Qt::UserRole).toLongLong() : 0;
}

int FolderDialog::rowForFolder(qint64 id) const
{
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        if (m_tree->topLevelItem(i)->data(kPathCol, Qt::UserRole).toLongLong() == id) return i;
    return -1;
}

void FolderDialog::selectFolder(qint64 id)
{
    const int row = rowForFolder(id);
    if (row >= 0) m_tree->setCurrentItem(m_tree->topLevelItem(row));
}

QString FolderDialog::statusOf(int row) const { return m_tree->topLevelItem(row)->text(kStateCol); }
QString FolderDialog::message() const { return m_message->text(); }

void FolderDialog::reload()
{
    const qint64 keep = currentId();
    m_tree->clear();
    Library lib(m_db);
    for (const FolderInfo& f : lib.folders()) {
        auto* it = new QTreeWidgetItem(m_tree);
        it->setData(kPathCol, Qt::UserRole, f.id);
        it->setText(kPathCol, f.path);
        it->setToolTip(kPathCol, f.path);
        it->setText(kCountCol, tr("%1本").arg(lib.recordingCountInFolder(f.id)));
        it->setText(kRecursiveCol, f.recursive ? tr("含む") : tr("—"));
        const QString st = m_state.value(f.id);
        it->setText(kStateCol, st.isEmpty() ? (f.lastScanAt ? tr("● 同期済") : tr("未走査")) : st);
        if (st.startsWith("⚠")) it->setForeground(kStateCol, QColor("#D64545"));
    }
    if (keep) selectFolder(keep);
    else if (m_tree->topLevelItemCount() > 0) m_tree->setCurrentItem(m_tree->topLevelItem(0));
    updateButtons();
}

void FolderDialog::updateButtons()
{
    const qint64 id = currentId();
    m_remove->setEnabled(id != 0);
    m_rescan->setEnabled(id != 0);
    m_exclude->setEnabled(id != 0);
    m_recursive->setEnabled(id != 0);
    QSignalBlocker b1(m_exclude), b2(m_recursive);
    if (!id) { m_exclude->clear(); m_recursive->setChecked(false); return; }
    Library lib(m_db);
    for (const FolderInfo& f : lib.folders())
        if (f.id == id) { m_exclude->setText(f.excludeGlob); m_recursive->setChecked(f.recursive); }
}

void FolderDialog::showMessage(const QString& text, bool error)
{
    m_message->setText(text);
    m_message->setProperty("error", error);
    m_message->style()->unpolish(m_message);
    m_message->style()->polish(m_message);
}

void FolderDialog::addFolder()
{
    const QString path = m_chooser ? m_chooser() : QString();
    if (path.isEmpty()) return;                       // cancelled
    Library lib(m_db);
    QString err;
    const qint64 id = lib.addFolder(path, true, QString(), &err);
    if (!id) { showMessage(tr("追加できません: %1").arg(err), true); return; }
    showMessage(tr("追加しました。録音を探しています…"), false);
    reload();
    selectFolder(id);
    emit libraryChanged();
    m_scan.scanFolder(id);
}

void FolderDialog::removeFolder()
{
    const qint64 id = currentId();
    if (!id) return;
    Library lib(m_db);
    const int n = lib.recordingCountInFolder(id);
    const QString path = m_tree->currentItem()->text(kPathCol);
    if (m_confirm && !m_confirm(tr("フォルダを削除"),
                                tr("「%1」をライブラリから外します。\nこのフォルダの録音 %2 本の文字起こし・要約・タグ・メモも削除されます。\n（音声ファイルそのものは削除されません。）").arg(path).arg(n)))
        return;
    if (!lib.removeFolder(id)) { showMessage(tr("削除できませんでした"), true); return; }
    m_state.remove(id);
    showMessage(tr("削除しました: %1").arg(path), false);
    reload();
    emit libraryChanged();
}

void FolderDialog::rescan()
{
    const qint64 id = currentId();
    if (id) { showMessage(tr("再走査しています…"), false); m_scan.scanFolder(id); }
}

void FolderDialog::applyExclude()
{
    const qint64 id = currentId();
    if (!id) return;
    Library lib(m_db);
    bool recursive = true;
    QString old;
    for (const FolderInfo& f : lib.folders()) if (f.id == id) { recursive = f.recursive; old = f.excludeGlob; }
    const QString now = m_exclude->text().trimmed();
    if (now == old) return;                           // nothing changed: no rescan
    if (lib.setFolderOptions(id, recursive, now)) { showMessage(tr("除外パターンを更新しました。再走査しています…"), false); m_scan.scanFolder(id); }
}

void FolderDialog::onScanStarted(qint64 id)
{
    m_state[id] = tr("◐ 走査中…");
    const int row = rowForFolder(id);
    if (row >= 0) m_tree->topLevelItem(row)->setText(kStateCol, m_state[id]);
}

void FolderDialog::onScanFinished(qint64 id, const ScanResult& r)
{
    if (r.ok()) {
        m_state[id] = tr("● 同期済");
        if (r.added || r.updated || r.moved || r.removed)
            showMessage(tr("走査完了: 追加 %1 ・ 更新 %2 ・ 移動 %3 ・ 削除 %4").arg(r.added).arg(r.updated).arg(r.moved).arg(r.removed), false);
        else showMessage(tr("走査完了: 変更はありません"), false);
    } else {
        m_state[id] = tr("⚠ %1").arg(r.error.contains("not accessible") ? tr("フォルダにアクセスできません（取り外し中?）") : r.error);
        showMessage(m_state[id], true);
    }
    reload();
    emit libraryChanged();
}

} // namespace ake
