#include "ui/DetailPane.h"

#include <QEvent>
#include <QFocusEvent>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "core/Database.h"
#include "core/RecordingEditor.h"
#include "ui/CardDelegate.h"
#include "ui/FlowLayout.h"

namespace ake {

namespace {
// Focus leaving because the whole window lost activation (alt-tab, closing): the user did not finish editing.
bool windowDeactivated(QEvent* e)
{
    return e->type() == QEvent::FocusOut && static_cast<QFocusEvent*>(e)->reason() == Qt::ActiveWindowFocusReason;
}
QLabel* label(const QString& text, const char* objectName, QWidget* parent = nullptr)
{
    auto* l = new QLabel(text, parent);
    l->setObjectName(objectName);
    return l;
}
QToolButton* tool(const QString& text, const QString& tip)
{
    auto* b = new QToolButton;
    b->setObjectName("flatTool");
    b->setText(text);
    b->setToolTip(tip);
    b->setCursor(Qt::PointingHandCursor);
    b->setAutoRaise(true);
    return b;
}
QPushButton* link(const QString& text)
{
    auto* b = new QPushButton(text);
    b->setObjectName("linkButton");
    b->setCursor(Qt::PointingHandCursor);
    b->setFlat(true);
    return b;
}
QString statusText(const QString& s)
{
    if (s == "done") return QObject::tr("解析済");
    if (s == "new") return QObject::tr("未解析");
    if (s == "queued") return QObject::tr("待機中");
    if (s == "transcribing" || s == "analyzing") return QObject::tr("解析中");
    if (s == "error") return QObject::tr("エラー");
    return s;
}
} // namespace

DetailPane::DetailPane(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("detailPane");
    setAttribute(Qt::WA_StyledBackground, true);
    build();
    showRecording(0);
}

DetailPane::~DetailPane()
{
    saveMemoNow();            // a memo typed just before closing is kept
    m_closing = true;         // children are about to be destroyed: no more edits from focus-out events
    for (QObject* o : { static_cast<QObject*>(m_titleEdit), static_cast<QObject*>(m_kwEdit), static_cast<QObject*>(m_memo),
                        static_cast<QObject*>(m_summaryEdit), static_cast<QObject*>(m_title) })
        if (o) o->removeEventFilter(this);
}

void DetailPane::aboutToClose()
{
    saveMemoNow();
    m_closing = true;
}

void DetailPane::setDatabase(Database* db, const ITokenizer* tokenizer)
{
    saveMemoNow();
    m_db = db;
    m_tok = tokenizer;
    m_editor = (db && tokenizer) ? std::make_unique<RecordingEditor>(*db, *tokenizer) : nullptr;
    reload();
}

void DetailPane::setTheme(const Theme& t)
{
    m_theme = t;
    if (m_id) fill();
}

void DetailPane::setReanalyzeEnabled(bool on)
{
    m_reanalyzeOk = on;
    if (m_reanalyzeBtn) m_reanalyzeBtn->setEnabled(on && m_id != 0);
}

// ------------------------------------------------------------------ construction
void DetailPane::build()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    m_pages = new QStackedWidget;
    root->addWidget(m_pages);

    m_empty = label(tr("録音を選択すると詳細が表示されます"), "placeholder");
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_pages->addWidget(m_empty);

    auto* content = new QWidget;
    auto* outer = new QVBoxLayout(content);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* scroll = new QScrollArea;
    scroll->setObjectName("detailScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* inner = new QWidget;
    inner->setObjectName("detailInner");
    scroll->setWidget(inner);
    auto* v = new QVBoxLayout(inner);
    v->setContentsMargins(18, 16, 18, 12);
    v->setSpacing(6);

    // title row
    auto* titleRow = new QHBoxLayout;
    m_titleBox = new QWidget;                 // label and editor share the spot; only one is shown (a hidden widget takes no space)
    auto* titleBoxLayout = new QVBoxLayout(m_titleBox);
    titleBoxLayout->setContentsMargins(0, 0, 0, 0);
    m_title = label(QString(), "detailTitle");
    m_title->setWordWrap(true);
    m_titleEdit = new QLineEdit;
    m_titleEdit->setObjectName("titleEdit");
    titleBoxLayout->addWidget(m_title);
    titleBoxLayout->addWidget(m_titleEdit);
    m_titleEdit->hide();
    m_titleBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_starBtn = tool("☆", tr("お気に入り"));
    m_starBtn->setCheckable(true);
    m_titleEditBtn = tool("✎", tr("タイトルを編集"));
    titleRow->addWidget(m_titleBox, 1);
    titleRow->addWidget(m_starBtn, 0, Qt::AlignTop);
    titleRow->addWidget(m_titleEditBtn, 0, Qt::AlignTop);
    v->addLayout(titleRow);

    auto* badgeRow = new QHBoxLayout;
    m_titleBadge = label(QString(), "badge");
    m_titleRevert = link(tr("AI版に戻す"));
    badgeRow->addWidget(m_titleBadge);
    badgeRow->addWidget(m_titleRevert);
    badgeRow->addStretch();
    v->addLayout(badgeRow);

    m_path = label(QString(), "detailSub");
    m_meta = label(QString(), "detailSub");
    m_meta->setTextFormat(Qt::RichText);
    v->addWidget(m_path);
    v->addWidget(m_meta);
    auto* sep = new QFrame;
    sep->setObjectName("detailSeparator");
    sep->setFixedHeight(1);
    v->addSpacing(6);
    v->addWidget(sep);
    v->addSpacing(4);

    // summary
    auto* sumRow = new QHBoxLayout;
    sumRow->addWidget(label(tr("要約"), "detailSection"));
    m_summaryBadge = label(QString(), "badge");
    sumRow->addWidget(m_summaryBadge);
    sumRow->addStretch();
    m_summaryRevert = link(tr("AI版に戻す"));
    m_summaryEditBtn = tool("✎", tr("要約を編集"));
    sumRow->addWidget(m_summaryRevert);
    sumRow->addWidget(m_summaryEditBtn);
    v->addLayout(sumRow);
    m_summaryBox = new QWidget;
    auto* summaryBoxLayout = new QVBoxLayout(m_summaryBox);
    summaryBoxLayout->setContentsMargins(0, 0, 0, 0);
    m_summary = label(QString(), "detailSummary");
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_summary->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_summaryEditor = new QWidget;
    auto* se = new QVBoxLayout(m_summaryEditor);
    se->setContentsMargins(0, 0, 0, 0);
    m_summaryEdit = new QPlainTextEdit;
    m_summaryEdit->setMinimumHeight(110);
    auto* seRow = new QHBoxLayout;
    m_summarySave = new QPushButton(tr("保存"));
    m_summarySave->setObjectName("primary");
    auto* seCancel = new QPushButton(tr("キャンセル"));
    seRow->addStretch();
    seRow->addWidget(seCancel);
    seRow->addWidget(m_summarySave);
    se->addWidget(m_summaryEdit);
    se->addLayout(seRow);
    summaryBoxLayout->addWidget(m_summary);
    summaryBoxLayout->addWidget(m_summaryEditor);
    m_summaryEditor->hide();
    v->addWidget(m_summaryBox);
    v->addSpacing(6);

    // keywords
    v->addWidget(label(tr("キーワード"), "detailSection"));
    m_chipHost = new QWidget;
    m_chips = new FlowLayout(m_chipHost, 6, 6);
    m_chips->setContentsMargins(0, 2, 0, 2);
    v->addWidget(m_chipHost);
    m_addKw = new QPushButton(tr("＋ 追加"), m_chipHost);       // both live in the chip area from the start
    m_addKw->setObjectName("chipAdd");
    m_addKw->setCursor(Qt::PointingHandCursor);
    m_kwEdit = new QLineEdit(m_chipHost);
    m_kwEdit->setObjectName("keywordEdit");
    m_kwEdit->setPlaceholderText(tr("キーワード"));
    m_kwEdit->setFixedWidth(130);
    m_kwEdit->hide();
    v->addSpacing(6);

    // memo
    v->addWidget(label(tr("メモ"), "detailSection"));
    m_memo = new QPlainTextEdit;
    m_memo->setObjectName("memoEdit");
    m_memo->setPlaceholderText(tr("自分用のメモ（検索対象になります）"));
    m_memo->setMinimumHeight(90);
    m_memo->setMaximumHeight(160);
    v->addWidget(m_memo);
    v->addStretch();
    outer->addWidget(scroll, 1);

    // bottom actions
    auto* bar = new QWidget;
    bar->setObjectName("detailBar");
    auto* br = new QHBoxLayout(bar);
    br->setContentsMargins(18, 10, 18, 12);
    m_openBtn = new QPushButton(tr("全文を開く"));
    m_openBtn->setObjectName("primary");
    m_playBtn = new QPushButton(tr("▶ 再生"));
    m_reanalyzeBtn = link(tr("再解析"));
    br->addWidget(m_openBtn);
    br->addWidget(m_playBtn);
    br->addStretch();
    br->addWidget(m_reanalyzeBtn);
    outer->addWidget(bar);
    m_pages->addWidget(content);

    m_memoTimer = new QTimer(this);
    m_memoTimer->setSingleShot(true);
    m_memoTimer->setInterval(kMemoSaveDelayMs);

    // --- behaviour
    connect(m_titleEditBtn, &QToolButton::clicked, this, [this] { beginTitleEdit(); });
    connect(m_titleEdit, &QLineEdit::returnPressed, this, [this] { commitTitleEdit(); });
    connect(m_titleRevert, &QPushButton::clicked, this, [this] { if (canEdit() && m_editor->revertTitle(m_id)) edited(); });
    connect(m_starBtn, &QToolButton::clicked, this, [this](bool on) {
        if (canEdit() && m_editor->setFavorite(m_id, on)) edited();
        else if (m_id) fill();
    });
    connect(m_summaryEditBtn, &QToolButton::clicked, this, [this] { beginSummaryEdit(); });
    connect(m_summarySave, &QPushButton::clicked, this, [this] { endSummaryEdit(true); });
    connect(seCancel, &QPushButton::clicked, this, [this] { endSummaryEdit(false); });
    connect(m_summaryRevert, &QPushButton::clicked, this, [this] { if (canEdit() && m_editor->revertSummary(m_id)) edited(); });
    connect(m_addKw, &QPushButton::clicked, this, [this] { beginKeywordAdd(); });
    connect(m_kwEdit, &QLineEdit::returnPressed, this, [this] { endKeywordAdd(true); });
    connect(m_memo, &QPlainTextEdit::textChanged, this, [this] { if (m_id && m_memo->toPlainText() != m_memoLoaded) m_memoTimer->start(); });
    connect(m_memoTimer, &QTimer::timeout, this, [this] { saveMemoNow(); });
    connect(m_openBtn, &QPushButton::clicked, this, [this] { if (m_id) emit openTranscriptRequested(m_id, 0); });
    connect(m_playBtn, &QPushButton::clicked, this, [this] { if (m_id) emit playRequested(m_id); });
    connect(m_reanalyzeBtn, &QPushButton::clicked, this, [this] { if (m_id) emit reanalyzeRequested(m_id); });

    m_titleEdit->installEventFilter(this);
    m_kwEdit->installEventFilter(this);
    m_memo->installEventFilter(this);
    m_summaryEdit->installEventFilter(this);
    m_title->installEventFilter(this);   // double-click edits the title
}

void DetailPane::showTitleEditor(bool on)
{
    m_title->setVisible(!on);
    m_titleEdit->setVisible(on);
}

void DetailPane::showSummaryEditor(bool on)
{
    m_summary->setVisible(!on);
    m_summaryEditor->setVisible(on);
}

// ------------------------------------------------------------------ showing
void DetailPane::showRecording(qint64 id)
{
    if (id != m_id) {
        saveMemoNow();                       // never lose a pending memo when the selection moves on
        if (m_titleEditing) cancelTitleEdit();
        if (summaryEditorOpen()) showSummaryEditor(false);
        endKeywordAdd(false);
        m_id = id;
        m_memoLoaded.clear();
        m_memo->blockSignals(true);
        m_memo->clear();
        m_memo->blockSignals(false);
    }
    reload();
}

void DetailPane::reload()
{
    m_detail = (m_id && m_db) ? RecordingDetail::load(*m_db, m_id) : RecordingDetail();
    if (m_id && !m_detail.valid()) m_id = 0;         // the recording was deleted
    fill();
}

void DetailPane::fill()
{
    if (!m_detail.valid()) { m_pages->setCurrentIndex(0); return; }
    m_pages->setCurrentIndex(1);
    const RecordingDetail& d = m_detail;
    const bool editable = canEdit();

    const QString fileName = QFileInfo(d.relPath).fileName();
    if (!m_titleEditing) m_title->setText(d.title.exists ? d.title.content : fileName);
    m_starBtn->setChecked(d.favorite);
    m_starBtn->setText(d.favorite ? "★" : "☆");
    m_starBtn->setProperty("on", d.favorite);
    m_starBtn->style()->unpolish(m_starBtn);
    m_starBtn->style()->polish(m_starBtn);
    m_starBtn->setEnabled(editable);
    m_titleEditBtn->setEnabled(editable);
    m_titleBadge->setVisible(d.title.exists);
    m_titleBadge->setText(d.title.isUser ? tr("編集済み") : tr("AI生成"));
    m_titleBadge->setProperty("user", d.title.isUser);
    m_titleBadge->style()->unpolish(m_titleBadge);
    m_titleBadge->style()->polish(m_titleBadge);
    m_titleRevert->setVisible(editable && d.title.isUser && d.title.aiAvailable);

    m_path->setToolTip(d.absPath);
    m_path->setProperty("full", d.relPath);
    const QFontMetrics fm(m_path->font());
    m_path->setText(fm.elidedText(d.relPath, Qt::ElideMiddle, qMax(80, width() - 40)));

    const QString dotColor = d.status == "done" ? m_theme.green.name() : d.status == "error" ? m_theme.red.name() : m_theme.accent.name();
    QStringList meta;
    meta << d.recordedAt.toString("yyyy-MM-dd HH:mm") << CardDelegate::formatDuration(d.durationMs);
    if (!d.language.isEmpty()) meta << d.language;
    m_meta->setText(meta.join(" ・ ") + QStringLiteral(" ・ <span style='color:%1'>●</span> %2").arg(dotColor, statusText(d.status)));

    // summary area
    const bool editingSummary = summaryEditorOpen();
    if (!editingSummary) {
        QString text;
        bool faint = false;
        if (d.status == "done") {
            text = d.summary.exists ? d.summary.content : tr("（この録音には要約がありません）");
            faint = !d.summary.exists;
        } else if (d.status == "error") {
            text = d.error.isEmpty() ? tr("解析に失敗しました") : d.error;
        } else {
            text = statusText(d.status) == tr("未解析") ? tr("まだ解析されていません。「再解析」で文字起こしを始めます。") : tr("%1…").arg(statusText(d.status));
            faint = true;
        }
        m_summary->setText(text);
        m_summary->setProperty("faint", faint);
        m_summary->setProperty("error", d.status == "error");
        m_summary->style()->unpolish(m_summary);
        m_summary->style()->polish(m_summary);
    }
    m_summaryBadge->setVisible(d.summary.exists && d.status == "done");
    m_summaryBadge->setText(d.summary.isUser ? tr("編集済み") : tr("AI生成"));
    m_summaryBadge->setProperty("user", d.summary.isUser);
    m_summaryBadge->style()->unpolish(m_summaryBadge);
    m_summaryBadge->style()->polish(m_summaryBadge);
    m_summaryEditBtn->setEnabled(editable && d.status == "done");
    m_summaryRevert->setVisible(editable && d.summary.isUser && d.summary.aiAvailable);

    rebuildChips();

    if (m_memo->toPlainText() == m_memoLoaded && !m_memoTimer->isActive()) {    // do not clobber what is being typed
        m_memoLoaded = d.notes;
        if (m_memo->toPlainText() != d.notes) {
            m_memo->blockSignals(true);
            m_memo->setPlainText(d.notes);
            m_memo->blockSignals(false);
        }
    }
    m_memo->setReadOnly(!editable);

    m_openBtn->setEnabled(d.segmentCount > 0);
    m_playBtn->setEnabled(true);
    m_reanalyzeBtn->setEnabled(m_reanalyzeOk);
    m_reanalyzeBtn->setText(d.status == "new" || d.status == "error" ? tr("解析する") : tr("再解析"));
}

void DetailPane::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    const QString full = m_path ? m_path->property("full").toString() : QString();
    if (!full.isEmpty()) m_path->setText(QFontMetrics(m_path->font()).elidedText(full, Qt::ElideMiddle, qMax(80, width() - 40)));
}

void DetailPane::rebuildChips()
{
    // The add button and the inline editor persist; only the chips are rebuilt.
    while (QLayoutItem* it = m_chips->takeAt(0)) {
        if (QWidget* w = it->widget(); w && w != m_addKw && w != m_kwEdit) delete w;
        delete it;
    }
    for (const RecordingDetail::Kw& k : m_detail.keywords) {
        auto* chip = new QFrame(m_chipHost);
        chip->setObjectName("chip");
        chip->setProperty("user", k.isUser);
        auto* h = new QHBoxLayout(chip);
        h->setContentsMargins(10, 2, 4, 2);
        h->setSpacing(4);
        auto* text = new QLabel(k.name, chip);
        text->setObjectName("chipText");
        auto* x = new QToolButton(chip);
        x->setObjectName("chipRemove");
        x->setText("✕");
        x->setAutoRaise(true);
        x->setCursor(Qt::PointingHandCursor);
        x->setToolTip(tr("このキーワードを外す"));
        x->setProperty("keywordName", k.name);
        x->setEnabled(canEdit());
        const qint64 kid = k.id;
        connect(x, &QToolButton::clicked, this, [this, kid] { if (canEdit() && m_editor->removeKeyword(m_id, kid)) edited(); });
        h->addWidget(text);
        h->addWidget(x);
        m_chips->addWidget(chip);
        chip->show();
    }
    m_addKw->setEnabled(canEdit());
    m_chips->addWidget(m_addingKeyword ? static_cast<QWidget*>(m_kwEdit) : static_cast<QWidget*>(m_addKw));
    m_addKw->setVisible(!m_addingKeyword);
    m_kwEdit->setVisible(m_addingKeyword);
    m_chipHost->updateGeometry();
}

// ------------------------------------------------------------------ editing
void DetailPane::edited()
{
    reload();
    emit recordingEdited(m_id);
}

void DetailPane::beginTitleEdit()
{
    if (!canEdit() || !m_id) return;
    m_titleEditing = true;
    m_titleEdit->setText(m_title->text());
    showTitleEditor(true);
    m_titleEdit->setFocus();
    m_titleEdit->selectAll();
}

void DetailPane::commitTitleEdit()
{
    if (!m_titleEditing) return;
    const QString text = m_titleEdit->text();
    m_titleEditing = false;
    showTitleEditor(false);
    // only a real change is saved (otherwise opening and leaving the editor would turn the AI title into a "user" one)
    if (text.simplified() != m_title->text().simplified() && canEdit() && m_editor->setTitle(m_id, text)) edited();
    else fill();
}

void DetailPane::cancelTitleEdit()
{
    m_titleEditing = false;
    showTitleEditor(false);
    fill();
}

void DetailPane::beginSummaryEdit()
{
    if (!canEdit() || !m_id || m_detail.status != "done") return;
    m_summaryEdit->setPlainText(m_detail.summary.exists ? m_detail.summary.content : QString());
    showSummaryEditor(true);
    m_summaryEdit->setFocus();
}

void DetailPane::endSummaryEdit(bool save)
{
    const QString text = m_summaryEdit->toPlainText();
    showSummaryEditor(false);
    if (save && canEdit() && text.trimmed() != m_detail.summary.content.trimmed() && m_editor->setSummary(m_id, text)) edited();
    else fill();
}

void DetailPane::beginKeywordAdd()
{
    if (!canEdit() || !m_id) return;
    m_addingKeyword = true;
    m_kwEdit->clear();
    rebuildChips();
    m_kwEdit->setFocus();
}

void DetailPane::endKeywordAdd(bool add)
{
    if (!m_addingKeyword) return;
    const QString name = m_kwEdit->text();
    m_addingKeyword = false;
    m_kwEdit->clear();
    if (add && !name.simplified().isEmpty() && canEdit() && m_editor->addKeyword(m_id, name)) edited();
    else rebuildChips();
}

void DetailPane::saveMemoNow()
{
    m_memoTimer->stop();
    if (!m_id || !canEdit()) return;
    const QString text = m_memo->toPlainText();
    if (text == m_memoLoaded) return;
    if (m_editor->setNotes(m_id, text)) {
        m_memoLoaded = text;
        emit recordingEdited(m_id);       // the memo is searchable: the search row changed
    }
}

bool DetailPane::eventFilter(QObject* obj, QEvent* e)
{
    if (m_closing) return false;
    if (obj == m_title && e->type() == QEvent::MouseButtonDblClick) {
        // not inside the event: hiding the label while it is still receiving the click sequence crashes its text control
        QTimer::singleShot(0, this, [this] { beginTitleEdit(); });
        return true;
    }
    if (obj == m_titleEdit) {
        if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) { cancelTitleEdit(); return true; }
        if (e->type() == QEvent::FocusOut && m_titleEditing && !windowDeactivated(e)) { commitTitleEdit(); return false; }
    } else if (obj == m_kwEdit) {
        if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) { endKeywordAdd(false); return true; }
        if (e->type() == QEvent::FocusOut && !windowDeactivated(e)) { endKeywordAdd(!m_kwEdit->text().trimmed().isEmpty()); return false; }
    } else if (obj == m_memo) {
        if (e->type() == QEvent::FocusOut) saveMemoNow();
    } else if (obj == m_summaryEdit && e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Escape) { endSummaryEdit(false); return true; }
        if ((k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && (k->modifiers() & Qt::ControlModifier)) { endSummaryEdit(true); return true; }
    }
    return QWidget::eventFilter(obj, e);
}

// ------------------------------------------------------------------ test accessors
bool DetailPane::showsRecording() const { return m_pages->currentIndex() == 1; }
QString DetailPane::titleText() const { return m_title->text(); }
QString DetailPane::summaryText() const { return m_summary->text(); }
QString DetailPane::memoText() const { return m_memo->toPlainText(); }
QString DetailPane::metaText() const { return m_meta->text(); }
QString DetailPane::badgeText(bool forTitle) const { return forTitle ? (m_titleBadge->isHidden() ? QString() : m_titleBadge->text()) : (m_summaryBadge->isHidden() ? QString() : m_summaryBadge->text()); }

QStringList DetailPane::keywordTexts() const
{
    QStringList l;
    for (const RecordingDetail::Kw& k : m_detail.keywords) l << k.name;
    return l;
}

QToolButton* DetailPane::removeButtonOfKeyword(const QString& name) const
{
    for (QToolButton* b : m_chipHost->findChildren<QToolButton*>("chipRemove"))
        if (b->property("keywordName").toString() == name) return b;
    return nullptr;
}

} // namespace ake
