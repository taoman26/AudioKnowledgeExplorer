#include "ui/SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/AnalysisPipeline.h"
#include "core/Settings.h"

namespace ake {

namespace {
// A combo box of preset numbers; a stored value that is not a preset stays selectable (set by hand / older version).
void fillInts(QComboBox* box, const QList<std::pair<int, QString>>& presets, int current, const std::function<QString(int)>& label)
{
    bool found = false;
    for (const auto& p : presets) {
        box->addItem(p.second, p.first);
        if (p.first == current) { box->setCurrentIndex(box->count() - 1); found = true; }
    }
    if (!found) { box->addItem(label(current), current); box->setCurrentIndex(box->count() - 1); }
}

QString sizeText(qint64 bytes) { return bytes >= (1 << 30) ? QStringLiteral("%1 GB").arg(bytes / double(1 << 30), 0, 'f', 1) : QStringLiteral("%1 MB").arg(bytes >> 20); }
}

SettingsDialog::SettingsDialog(Settings& settings, QWidget* parent)
    : QDialog(parent), m_settings(settings)
{
    setWindowTitle(tr("設定"));
    setMinimumWidth(560);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(24, 20, 24, 20);
    v->setSpacing(12);
    auto* head = new QLabel(tr("設定"));
    head->setObjectName("detailTitle");
    v->addWidget(head);

    auto section = [&](const QString& text) { auto* l = new QLabel(text); l->setObjectName("detailSection"); v->addWidget(l); };
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    form->setHorizontalSpacing(16);

    // --- transcription
    section(tr("文字起こし"));
    m_model = new QComboBox;
    m_model->addItem(tr("標準 (%1)").arg(QFileInfo(AnalysisConfig::defaultModelPath()).fileName()), QString());
    const QString current = settings.value(Settings::kModelPath).trimmed();
    bool found = current.isEmpty();
    for (const auto& m : Settings::installedModels(Settings::modelDirectory())) {
        m_model->addItem(QStringLiteral("%1  (%2)").arg(m.name, sizeText(m.sizeBytes)), m.path);
        if (m.path == current) { m_model->setCurrentIndex(m_model->count() - 1); found = true; }
    }
    if (!found) addCustomModel(current);
    auto* browse = new QPushButton(tr("参照…"));
    browse->setMinimumWidth(72);
    auto* modelRow = new QHBoxLayout;
    modelRow->addWidget(m_model, 1);
    modelRow->addWidget(browse);
    form->addRow(tr("モデル"), modelRow);
    m_modelNote = new QLabel;
    m_modelNote->setObjectName("folderMessage");
    m_modelNote->setWordWrap(true);
    m_modelNote->setMinimumHeight(m_modelNote->fontMetrics().lineSpacing() * 3 + 4);   // the warning is three lines; never clip it
    form->addRow(QString(), m_modelNote);

    m_language = new QComboBox;
    const std::pair<const char*, const char*> langs[] = { { "auto", "自動検出" }, { "ja", "日本語" }, { "en", "English" }, { "zh", "中文" },
                                                         { "ko", "한국어" }, { "es", "Español" }, { "fr", "Français" }, { "de", "Deutsch" } };
    const QString lang = settings.analysisConfig().language;
    bool langFound = false;
    for (const auto& l : langs) {
        m_language->addItem(QString::fromUtf8(l.second), QString::fromLatin1(l.first));
        if (lang == QLatin1String(l.first)) { m_language->setCurrentIndex(m_language->count() - 1); langFound = true; }
    }
    if (!langFound) { m_language->addItem(lang, lang); m_language->setCurrentIndex(m_language->count() - 1); }   // a code set by hand stays selectable
    form->addRow(tr("言語"), m_language);

    m_quality = new QComboBox;
    fillInts(m_quality, { { AnalysisConfig::kDefaultBeamSize, tr("精度優先(ビームサーチ・約1.5倍の時間)") }, { 0, tr("高速") } },
             settings.analysisConfig().beamSize, [](int n) { return QStringLiteral("ビーム幅 %1").arg(n); });
    form->addRow(tr("方式"), m_quality);

    m_threads = new QComboBox;
    fillInts(m_threads, { { 0, tr("自動") }, { 1, "1" }, { 2, "2" }, { 4, "4" }, { 6, "6" }, { 8, "8" }, { 12, "12" }, { 16, "16" } },
             settings.analysisConfig().threads, [](int n) { return QString::number(n); });
    form->addRow(tr("スレッド数"), m_threads);
    v->addLayout(form);

    // --- library
    section(tr("ライブラリ"));
    auto* form2 = new QFormLayout;
    form2->setLabelAlignment(Qt::AlignRight);
    form2->setHorizontalSpacing(16);
    m_auto = new QCheckBox(tr("新しい録音を自動で解析する"));
    m_auto->setChecked(settings.autoAnalyze());
    form2->addRow(QString(), m_auto);
    m_watch = new QCheckBox(tr("フォルダを常時監視して、新しい録音を見つける"));
    m_watch->setChecked(settings.watchFolders());
    form2->addRow(QString(), m_watch);
    m_scan = new QComboBox;
    fillInts(m_scan, { { 15, tr("15 秒ごと") }, { 60, tr("1 分ごと") }, { 300, tr("5 分ごと") },
                       { 900, tr("15 分ごと") }, { 3600, tr("1 時間ごと") } },
             settings.scanIntervalSeconds() > 0 ? settings.scanIntervalSeconds() : Settings::kDefaultScanIntervalS,
             [this](int sec) { return tr("%1 秒ごと").arg(sec); });
    form2->addRow(tr("確認の間隔"), m_scan);
    m_scan->setEnabled(m_watch->isChecked());
    connect(m_watch, &QCheckBox::toggled, m_scan, &QWidget::setEnabled);
    auto* watchNote = new QLabel(tr("監視しないときは、起動時と「ライブラリ ▸ フォルダを今すぐ確認」(F5)でだけ確認します。"));
    watchNote->setObjectName("folderMessage");
    watchNote->setWordWrap(true);
    form2->addRow(QString(), watchNote);
    v->addLayout(form2);

    // --- appearance
    section(tr("外観"));
    auto* form3 = new QFormLayout;
    form3->setLabelAlignment(Qt::AlignRight);
    form3->setHorizontalSpacing(16);
    m_theme = new QComboBox;
    m_theme->addItem(tr("システムに合わせる"), "system");
    m_theme->addItem(tr("ライト"), "light");
    m_theme->addItem(tr("ダーク"), "dark");
    m_theme->setCurrentIndex(m_theme->findData(settings.theme()));
    form3->addRow(tr("テーマ"), m_theme);
    v->addLayout(form3);
    auto* hint = new QLabel(tr("モデルとスレッド数の変更は、次に解析する録音から反映されます。"));
    hint->setObjectName("detailSub");
    v->addWidget(hint);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    auto* cancel = new QPushButton(tr("キャンセル"));
    m_ok = new QPushButton(tr("OK"));
    m_ok->setObjectName("primary");
    m_ok->setDefault(true);
    buttons->addWidget(cancel);
    buttons->addWidget(m_ok);
    v->addLayout(buttons);

    m_browser = [this] {
        return QFileDialog::getOpenFileName(this, tr("whisper モデル (ggml)"), Settings::modelDirectory(), tr("ggml モデル (*.bin);;すべてのファイル (*)"));
    };
    connect(browse, &QPushButton::clicked, this, [this] { const QString f = m_browser ? m_browser() : QString(); if (!f.isEmpty()) addCustomModel(f); });
    connect(m_model, &QComboBox::currentIndexChanged, this, [this] { updateModelNote(); });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_ok, &QPushButton::clicked, this, &QDialog::accept);
    updateModelNote();
}

void SettingsDialog::addCustomModel(const QString& path)
{
    int i = m_model->findData(path);
    if (i < 0) { m_model->addItem(QFileInfo(path).fileName() + "  (" + path + ")", path); i = m_model->count() - 1; }
    m_model->setCurrentIndex(i);
}

void SettingsDialog::updateModelNote()
{
    QString path = m_model->currentData().toString();
    if (path.isEmpty()) path = AnalysisConfig::defaultModelPath();
    const bool ok = QFileInfo(path).isFile();
    m_modelNote->setText(ok ? path : tr("モデルファイルが見つかりません: %1\n解析するには、このファイルを置くか、別のモデルを選んでください。").arg(path));
    m_modelNote->setProperty("error", !ok);
    m_modelNote->style()->unpolish(m_modelNote);
    m_modelNote->style()->polish(m_modelNote);
}

void SettingsDialog::accept()
{
    m_settings.set(Settings::kModelPath, m_model->currentData().toString());
    m_settings.set(Settings::kLanguage, m_language->currentData().toString());
    m_settings.setInt(Settings::kBeam, m_quality->currentData().toInt());
    m_settings.setInt(Settings::kThreads, m_threads->currentData().toInt());
    m_settings.setBool(Settings::kAutoAnalyze, m_auto->isChecked());
    m_settings.setBool(Settings::kScanWatch, m_watch->isChecked());
    m_settings.setInt(Settings::kScanInterval, m_scan->currentData().toInt());
    m_settings.set(Settings::kTheme, m_theme->currentData().toString());
    QDialog::accept();
}

} // namespace ake
