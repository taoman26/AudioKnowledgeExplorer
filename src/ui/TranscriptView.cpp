#include "ui/TranscriptView.h"

#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include "core/Database.h"
#include "core/ITokenizer.h"
#include "core/RecordingDetail.h"
#include "core/RecordingEditor.h"
#include "core/SearchService.h"
#include "ui/CardDelegate.h"
#include "ui/FlowLayout.h"
#include "ui/TranscriptModel.h"

namespace ake {

namespace {
constexpr int kTimeColumn = 92;      // px reserved for "00:12:40"
constexpr int kLineHeight = 22;
constexpr int kRowPadding = 7;
constexpr int kTextLeft(bool times) { return times ? 18 + kTimeColumn : 22; }
} // namespace

// ------------------------------------------------------------------ delegate
class TranscriptDelegate : public QStyledItemDelegate {
public:
    explicit TranscriptDelegate(QObject* parent) : QStyledItemDelegate(parent), theme(Theme::light()) {}
    Theme theme;
    bool showTimes = true;
    int playRow = -1;
    int editRow = -1;
    const TranscriptModel* model = nullptr;

    QFont textFont(const QFont& base) const { QFont f = base; f.setPixelSize(14); return f; }

    QList<CardDelegate::Line> lines(const QString& text, const QFont& base, int width) const
    {
        return CardDelegate::wrapElideLines(text, QFontMetrics(textFont(base)), qMax(60, width), 100000);
    }
    QSize sizeHint(const QStyleOptionViewItem& o, const QModelIndex& i) const override
    {
        const int w = o.rect.width() - kTextLeft(showTimes) - 28;
        const int n = qMax<int>(1, int(lines(i.data(Qt::DisplayRole).toString(), o.font, w).size()));
        return { o.rect.width(), qMax(34, n * kLineHeight + 2 * kRowPadding) };
    }
    void paint(QPainter* p, const QStyleOptionViewItem& o, const QModelIndex& i) const override
    {
        const Theme& t = theme;
        const bool playing = i.row() == playRow, selected = o.state & QStyle::State_Selected;
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        const QRect r = o.rect;
        if (playing) {
            p->fillRect(r.adjusted(8, 1, -8, -1), t.accentBg);
            p->fillRect(QRect(r.left() + 8, r.top() + 1, 4, r.height() - 2), t.accent);
        } else if (selected) {
            QColor c = t.line; c.setAlpha(110);
            p->fillRect(r.adjusted(8, 1, -8, -1), c);
        } else if (o.state & QStyle::State_MouseOver) {
            QColor c = t.line; c.setAlpha(60);
            p->fillRect(r.adjusted(8, 1, -8, -1), c);
        }
        if (showTimes) {
            QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            mono.setPixelSize(12);
            p->setFont(mono);
            p->setPen(playing ? t.accent : t.faint);
            p->drawText(QRect(r.left() + 22, r.top() + kRowPadding, kTimeColumn - 8, kLineHeight), Qt::AlignLeft | Qt::AlignVCenter,
                        TranscriptModel::formatTime(i.data(TranscriptModel::StartMsRole).toLongLong()));
        }
        if (i.row() != editRow) {                                   // the editor is drawn over this row while editing
            const QFont f = textFont(o.font);
            const QFontMetrics fm(f);
            const int left = r.left() + kTextLeft(showTimes), w = r.width() - kTextLeft(showTimes) - 28;
            const QString text = i.data(Qt::DisplayRole).toString();
            const auto ls = lines(text, o.font, w);
            const QVariantList hl = i.data(TranscriptModel::HighlightsRole).toList();
            const int cur = model ? model->currentHitStartIn(i.row()) : -1;
            p->setFont(f);
            for (int n = 0; n < ls.size(); ++n) {
                const int baseline = r.top() + kRowPadding + n * kLineHeight + (kLineHeight + fm.ascent() - fm.descent()) / 2;
                for (int k = 0; k + 1 < hl.size(); k += 2) {
                    const int hs = hl[k].toInt(), he = hs + hl[k + 1].toInt();
                    const int from = qMax(hs, ls[n].start), to = qMin(he, ls[n].start + int(ls[n].text.size()));
                    if (to <= from) continue;
                    QColor c = (hs == cur) ? t.accent : t.hl;
                    if (hs == cur) c.setAlpha(150);
                    p->setPen(Qt::NoPen);
                    p->setBrush(c);
                    p->drawRoundedRect(QRect(left + fm.horizontalAdvance(ls[n].text.left(from - ls[n].start)) - 1, baseline - fm.ascent(),
                                             fm.horizontalAdvance(ls[n].text.mid(from - ls[n].start, to - from)) + 2, fm.height()), 3, 3);
                }
                p->setPen(playing ? t.text : t.text);
                QFont ff = f; ff.setBold(playing);
                p->setFont(ff);
                p->drawText(QPoint(left, baseline), ls[n].text);
            }
        }
        if (i.data(TranscriptModel::EditedRole).toBool()) {           // marks a corrected sentence
            p->setPen(Qt::NoPen);
            p->setBrush(t.amber);
            p->drawEllipse(QPoint(r.right() - 16, r.top() + kRowPadding + kLineHeight / 2), 3, 3);
        }
        p->restore();
    }
};

// ------------------------------------------------------------------ construction
TranscriptView::TranscriptView(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("transcriptView");
    setAttribute(Qt::WA_StyledBackground, true);
    build();
}

TranscriptView::~TranscriptView() = default;

void TranscriptView::setDatabase(Database* db, const ITokenizer* tokenizer)
{
    m_db = db;
    m_tok = tokenizer;
    m_recEditor = (db && tokenizer) ? std::make_unique<RecordingEditor>(*db, *tokenizer) : nullptr;
}

void TranscriptView::setTheme(const Theme& t)
{
    m_theme = t;
    m_delegate->theme = t;
    m_list->viewport()->update();
}

static QToolButton* toggle(const QString& text, const QString& tip, bool on)
{
    auto* b = new QToolButton;
    b->setObjectName("toolToggle");
    b->setText(text);
    b->setToolTip(tip);
    b->setCheckable(true);
    b->setChecked(on);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

void TranscriptView::build()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // --- header
    auto* header = new QWidget;
    header->setObjectName("transcriptHeader");
    header->setAttribute(Qt::WA_StyledBackground, true);
    auto* h = new QHBoxLayout(header);
    h->setContentsMargins(12, 10, 12, 10);
    h->setSpacing(8);
    m_back = new QPushButton(tr("← 戻る"));
    m_title = new QLabel;
    m_title->setObjectName("detailTitle");
    m_meta = new QLabel;
    m_meta->setObjectName("detailSub");
    m_search = new QLineEdit;
    m_search->setObjectName("transcriptSearch");
    m_search->setPlaceholderText(tr("全文内を検索"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedWidth(260);
    m_counter = new QLabel;
    m_counter->setObjectName("searchCounter");
    m_counter->setMinimumWidth(52);
    m_counter->setAlignment(Qt::AlignCenter);
    m_prev = new QToolButton; m_prev->setText("▲"); m_prev->setObjectName("flatTool"); m_prev->setToolTip(tr("前の一致 (Shift+Enter)"));
    m_next = new QToolButton; m_next->setText("▼"); m_next->setObjectName("flatTool"); m_next->setToolTip(tr("次の一致 (Enter)"));
    m_timeToggle = toggle(tr("時刻"), tr("時刻を表示"), true);
    m_followToggle = toggle(tr("追従"), tr("再生位置に追従"), true);
    m_editToggle = toggle(tr("編集モード"), tr("クリックで文を編集"), false);
    m_copyBtn = new QToolButton;
    m_copyBtn->setObjectName("toolToggle");
    m_copyBtn->setText(tr("コピー ▾"));
    m_copyBtn->setPopupMode(QToolButton::InstantPopup);
    auto* copyMenu = new QMenu(m_copyBtn);
    copyMenu->addAction(tr("全文をコピー"), this, [this] { QApplication::clipboard()->setText(copyText(false)); });
    copyMenu->addAction(tr("時刻付きでコピー"), this, [this] { QApplication::clipboard()->setText(copyText(true)); });
    m_copyBtn->setMenu(copyMenu);
    h->addWidget(m_back);
    h->addWidget(m_title);
    h->addWidget(m_meta);
    h->addStretch(1);
    h->addWidget(m_search);
    h->addWidget(m_counter);
    h->addWidget(m_prev);
    h->addWidget(m_next);
    h->addSpacing(8);
    h->addWidget(m_timeToggle);
    h->addWidget(m_followToggle);
    h->addWidget(m_editToggle);
    h->addWidget(m_copyBtn);
    root->addWidget(header);

    // --- body: keywords | sentences
    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    auto* side = new QWidget;
    side->setObjectName("transcriptSide");
    side->setAttribute(Qt::WA_StyledBackground, true);
    side->setFixedWidth(220);
    auto* sv = new QVBoxLayout(side);
    sv->setContentsMargins(14, 14, 14, 12);
    auto* kwTitle = new QLabel(tr("キーワード"));
    kwTitle->setObjectName("detailSection");
    sv->addWidget(kwTitle);
    m_chipHost = new QWidget;
    m_chips = new FlowLayout(m_chipHost, 5, 5);
    m_chips->setContentsMargins(0, 4, 0, 0);
    sv->addWidget(m_chipHost);
    sv->addStretch(1);
    m_info = new QLabel;
    m_info->setObjectName("detailSub");
    m_info->setWordWrap(true);
    sv->addWidget(m_info);
    body->addWidget(side);

    m_model = new TranscriptModel(this);
    m_list = new QListView;
    m_list->setObjectName("transcriptList");
    m_list->setModel(m_model);
    m_delegate = new TranscriptDelegate(this);
    m_delegate->model = m_model;
    m_list->setItemDelegate(m_delegate);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setUniformItemSizes(false);
    m_list->setLayoutMode(QListView::SinglePass);   // all rows positioned at once: scrolling to a hit must not race a lazy layout
    m_list->setResizeMode(QListView::Adjust);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->verticalScrollBar()->setSingleStep(30);
    m_list->setMouseTracking(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    body->addWidget(m_list, 1);
    root->addLayout(body, 1);

    // --- behaviour
    connect(m_back, &QPushButton::clicked, this, [this] { if (isEditing()) commitEdit(); emit backRequested(); });
    connect(m_search, &QLineEdit::textChanged, this, [this] { applySearch(); });
    connect(m_next, &QToolButton::clicked, this, [this] { nextHit(); });
    connect(m_prev, &QToolButton::clicked, this, [this] { previousHit(); });
    connect(m_timeToggle, &QToolButton::toggled, this, [this](bool on) {
        m_delegate->showTimes = on;
        m_list->doItemsLayout();
        m_list->viewport()->update();
    });
    connect(m_list, &QListView::clicked, this, [this](const QModelIndex& i) { onRowClicked(i); });
    // Not inside the click: the button release that follows would move the focus back to the list and close the new editor.
    connect(m_list, &QListView::doubleClicked, this, [this](const QModelIndex& i) {
        const int row = i.row();
        QTimer::singleShot(0, this, [this, row] { beginEdit(row); });
    });
    connect(m_list, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) { showContextMenu(pos); });
    connect(m_model, &TranscriptModel::highlightsChanged, m_list->viewport(), qOverload<>(&QWidget::update));
    m_search->installEventFilter(this);
    m_list->viewport()->installEventFilter(this);

    auto* find = new QShortcut(QKeySequence::Find, this);
    find->setContext(Qt::WidgetWithChildrenShortcut);
    connect(find, &QShortcut::activated, this, [this] { m_search->setFocus(); m_search->selectAll(); });
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    esc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(esc, &QShortcut::activated, this, [this] {
        if (isEditing()) cancelEdit();
        else if (!m_search->text().isEmpty()) m_search->clear();
        else emit backRequested();
    });
}

// ------------------------------------------------------------------ opening
bool TranscriptView::open(qint64 id, qint64 startMs, const QString& query)
{
    if (!m_db) return false;
    if (isEditing()) commitEdit();
    const RecordingDetail d = RecordingDetail::load(*m_db, id);
    if (!d.valid()) return false;
    m_id = id;
    m_playRow = -1;
    m_delegate->playRow = -1;
    m_delegate->editRow = -1;
    m_model->load(*m_db, id);
    m_title->setText(d.title.exists ? d.title.content : QFileInfo(d.relPath).fileName());
    m_meta->setText(QStringLiteral("%1 ・ %2").arg(d.recordedAt.toString("yyyy-MM-dd HH:mm"), CardDelegate::formatDuration(d.durationMs)));
    m_info->setText(tr("%1 ・ %2 ・ %3発言").arg(d.model.isEmpty() ? tr("(モデル不明)") : d.model, d.language.isEmpty() ? "-" : d.language).arg(d.segmentCount));

    while (QLayoutItem* it = m_chips->takeAt(0)) { delete it->widget(); delete it; }
    for (const RecordingDetail::Kw& k : d.keywords) {
        auto* b = new QPushButton(k.name, m_chipHost);
        b->setObjectName("chipButton");
        b->setProperty("user", k.isUser);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tr("この語で全文内を検索"));
        const QString name = k.name;
        connect(b, &QPushButton::clicked, this, [this, name] { m_search->setText(name); });
        m_chips->addWidget(b);
        b->show();
    }

    QSignalBlocker block(m_search);
    m_search->setText(query);
    applySearch();
    if (startMs >= 0 && m_model->rowCount() > 0) {
        const int row = m_model->rowForTime(startMs);
        // with a query, land on the first hit at or after that sentence
        for (int h = 0; h < m_model->hits().size(); ++h)
            if (m_model->hits()[h].row >= row) { goToHit(h); return true; }
        scrollToRow(row, true);
        m_list->setCurrentIndex(m_model->index(row, 0));
    } else if (m_model->rowCount() > 0 && m_model->hits().isEmpty()) {
        m_list->scrollToTop();
    }
    return true;
}

// ------------------------------------------------------------------ search
void TranscriptView::setSearchText(const QString& text) { m_search->setText(text); }

void TranscriptView::applySearch()
{
    m_model->setSearchTerms(SearchService::parseTerms(m_search->text()));
    if (!m_model->hits().isEmpty()) goToHit(0);
    else updateCounter();
}

int TranscriptView::hitCount() const { return int(m_model->hits().size()); }
int TranscriptView::currentHitIndex() const { return m_model->currentHit(); }

void TranscriptView::updateCounter()
{
    if (m_search->text().trimmed().isEmpty()) { m_counter->clear(); return; }
    m_counter->setText(m_model->hits().isEmpty() ? tr("0 / 0") : QStringLiteral("%1 / %2").arg(m_model->currentHit() + 1).arg(m_model->hits().size()));
}

QString TranscriptView::counterText() const { return m_counter->text(); }

void TranscriptView::goToHit(int index)
{
    if (m_model->hits().isEmpty()) return;
    const int n = int(m_model->hits().size());
    index = ((index % n) + n) % n;                  // wraps around at both ends
    m_model->setCurrentHit(index);
    scrollToRow(m_model->hits()[index].row, true);
    updateCounter();
}

void TranscriptView::nextHit() { if (!m_model->hits().isEmpty()) goToHit(m_model->currentHit() + 1); }
void TranscriptView::previousHit() { if (!m_model->hits().isEmpty()) goToHit(m_model->currentHit() - 1); }

void TranscriptView::scrollToRow(int row, bool center)
{
    if (row < 0 || row >= m_model->rowCount()) return;
    m_list->scrollTo(m_model->index(row, 0), center ? QAbstractItemView::PositionAtCenter : QAbstractItemView::EnsureVisible);
}

// ------------------------------------------------------------------ playback / clicks
void TranscriptView::setPlaybackPositionMs(qint64 ms)
{
    const int row = m_model->rowForTime(ms);
    if (row == m_playRow) return;
    const int old = m_playRow;
    m_playRow = row;
    m_delegate->playRow = row;
    for (int r : { old, row })
        if (r >= 0) m_list->update(m_model->index(r, 0));
    if (m_followToggle->isChecked() && row >= 0 && !isEditing()) scrollToRow(row, true);
}

void TranscriptView::clearPlayback()
{
    if (m_playRow < 0) return;
    const int old = m_playRow;
    m_playRow = -1;
    m_delegate->playRow = -1;
    m_list->update(m_model->index(old, 0));
}

void TranscriptView::onRowClicked(const QModelIndex& idx)
{
    if (!idx.isValid()) return;
    if (m_editToggle->isChecked()) { beginEdit(idx.row()); return; }
    emit seekRequested(idx.data(TranscriptModel::StartMsRole).toLongLong());   // "文クリック → その時刻から再生"
}

QString TranscriptView::copyText(bool withTimes) const { return m_model->fullText(withTimes); }

// ------------------------------------------------------------------ editing
void TranscriptView::beginEdit(int row)
{
    if (!m_recEditor || row < 0 || row >= m_model->rowCount()) return;
    if (isEditing()) commitEdit();
    m_editRow = row;
    m_delegate->editRow = row;
    if (!m_editor) {
        m_editor = new QPlainTextEdit(m_list->viewport());
        m_editor->setObjectName("segmentEditor");
        m_editor->installEventFilter(this);
    }
    const QModelIndex idx = m_model->index(row, 0);
    m_list->scrollTo(idx, QAbstractItemView::EnsureVisible);
    const QRect r = m_list->visualRect(idx);
    m_editor->setPlainText(m_model->text(row));
    m_editor->setGeometry(r.left() + kTextLeft(m_delegate->showTimes) - 6, r.top() + 2, r.width() - kTextLeft(m_delegate->showTimes) - 20, qMax(r.height() - 4, 64));
    m_editor->show();
    m_editor->setFocus();
    m_editor->selectAll();
    m_list->viewport()->update();
}

bool TranscriptView::commitEdit()
{
    if (!isEditing()) return false;
    const int row = m_editRow;
    const QString text = m_editor->toPlainText().trimmed();
    m_editRow = -1;
    m_delegate->editRow = -1;
    m_editor->hide();
    m_list->setFocus();
    bool changed = false;
    if (text != m_model->text(row).trimmed() && m_recEditor) {
        QString err;
        if (m_recEditor->setSegmentText(m_id, m_model->segmentId(row), text, &err)) {
            // the recognized text is the one stored in the model's OriginalRole; equal text means the correction was cleared
            const bool edited = text != m_model->index(row, 0).data(TranscriptModel::OriginalRole).toString();
            m_model->setSegmentText(row, text, edited);
            m_list->doItemsLayout();
            changed = true;
            emit recordingEdited(m_id);
        } else {
            QToolTip::showText(QCursor::pos(), tr("保存できませんでした: %1").arg(err), this);
        }
    }
    m_list->viewport()->update();
    return changed;
}

void TranscriptView::cancelEdit()
{
    if (!isEditing()) return;
    m_editRow = -1;
    m_delegate->editRow = -1;
    m_editor->hide();
    m_list->setFocus();
    m_list->viewport()->update();
}

bool TranscriptView::revertRow(int row)
{
    if (!m_recEditor || row < 0 || row >= m_model->rowCount() || !m_model->isEdited(row)) return false;
    QString err;
    if (!m_recEditor->revertSegment(m_id, m_model->segmentId(row), &err)) return false;
    m_model->setSegmentText(row, m_model->index(row, 0).data(TranscriptModel::OriginalRole).toString(), false);
    m_list->doItemsLayout();
    emit recordingEdited(m_id);
    return true;
}

void TranscriptView::showContextMenu(const QPoint& pos)
{
    const QModelIndex idx = m_list->indexAt(pos);
    if (!idx.isValid()) return;
    const int row = idx.row();
    QMenu menu(this);
    menu.addAction(tr("ここから再生"), this, [this, idx] { emit seekRequested(idx.data(TranscriptModel::StartMsRole).toLongLong()); });
    QAction* edit = menu.addAction(tr("この文を編集"), this, [this, row] { beginEdit(row); });
    edit->setEnabled(m_recEditor != nullptr);
    QAction* rev = menu.addAction(tr("元の文に戻す"), this, [this, row] { revertRow(row); });
    rev->setEnabled(m_recEditor && m_model->isEdited(row));
    menu.addAction(tr("この文をコピー"), this, [this, idx] { QApplication::clipboard()->setText(idx.data(Qt::DisplayRole).toString()); });
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

bool TranscriptView::eventFilter(QObject* obj, QEvent* e)
{
    if (m_closing) return false;
    if (obj == m_editor && e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Escape) { cancelEdit(); return true; }
        if ((k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && !(k->modifiers() & Qt::ShiftModifier)) { commitEdit(); return true; }
    } else if (obj == m_editor && e->type() == QEvent::FocusOut) {
        if (isEditing() && static_cast<QFocusEvent*>(e)->reason() != Qt::ActiveWindowFocusReason) commitEdit();   // clicking elsewhere saves
    } else if (obj == m_search && e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
            if (k->modifiers() & Qt::ShiftModifier) previousHit(); else nextHit();
            return true;
        }
    } else if (obj == m_list->viewport() && e->type() == QEvent::Resize && isEditing()) {
        cancelEdit();                                    // the row moved under the editor: do not leave it floating
    }
    return QWidget::eventFilter(obj, e);
}

// ------------------------------------------------------------------ test accessors
QString TranscriptView::titleText() const { return m_title->text(); }
QString TranscriptView::infoText() const { return m_info->text(); }

QStringList TranscriptView::keywordTexts() const
{
    QStringList l;
    for (QPushButton* b : m_chipHost->findChildren<QPushButton*>("chipButton")) l << b->text();
    return l;
}

QPushButton* TranscriptView::keywordButton(const QString& name) const
{
    for (QPushButton* b : m_chipHost->findChildren<QPushButton*>("chipButton"))
        if (b->text() == name) return b;
    return nullptr;
}

} // namespace ake
