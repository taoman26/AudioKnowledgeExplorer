#pragma once
#include <QDialog>
#include <functional>

#include "core/Settings.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

namespace ake {



// Whisper model / language / threads, automatic analysis and scanning, theme. Nothing is stored until OK.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(Settings& settings, QWidget* parent = nullptr);

    void accept() override;                    // writes the values to the settings

    // for tests
    QComboBox* modelCombo() const { return m_model; }
    QComboBox* languageCombo() const { return m_language; }
    QComboBox* qualityCombo() const { return m_quality; }       // data = beam width: 5 accurate, 0 fast
    QComboBox* threadsCombo() const { return m_threads; }       // data = thread count, 0 = automatic
    QCheckBox* autoAnalyzeBox() const { return m_auto; }
    QComboBox* scanIntervalCombo() const { return m_scan; }     // data = seconds, 0 = never
    QComboBox* themeCombo() const { return m_theme; }
    QLabel* modelNote() const { return m_modelNote; }
    QPushButton* okButton() const { return m_ok; }
    void addCustomModel(const QString& path);  // what the "参照…" button does after the file was chosen
    void setBrowser(std::function<QString()> b) { m_browser = std::move(b); }

private:
    void updateModelNote();

    Settings m_settings;                       // a handle on the database: cheap to copy
    QComboBox* m_model;
    QComboBox* m_language;
    QComboBox* m_quality;
    QComboBox* m_threads;
    QCheckBox* m_auto;
    QComboBox* m_scan;
    QComboBox* m_theme;
    QLabel* m_modelNote;
    QPushButton* m_ok;
    std::function<QString()> m_browser;
};

} // namespace ake
