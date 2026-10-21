#pragma once
#include <QList>
#include <QString>

#include "core/AnalysisPipeline.h"

namespace ake {

class Database;

// User settings, stored in the library database (`settings` table: key/value). Every key has a default, so an empty
// table is a valid configuration and a damaged value never breaks start-up.
class Settings {
public:
    // keys
    static constexpr const char* kModelPath = "whisper.model_path";   // empty = default location
    static constexpr const char* kLanguage = "whisper.language";      // auto | ja | en | ...
    static constexpr const char* kThreads = "whisper.threads";        // 0 = automatic
    static constexpr const char* kBeam = "whisper.beam";              // beam width; 0 = fast greedy decoding
    static constexpr const char* kSuppressNst = "whisper.suppress_nst";   // forbid "(音声)"-like tokens (default on)
    static constexpr const char* kAutoAnalyze = "analysis.auto";      // analyse newly found recordings by itself
    static constexpr const char* kScanWatch = "scan.watch";           // keep watching the folders (rescan every kScanInterval); off: only at start and on request
    static constexpr const char* kScanInterval = "scan.interval_s";   // rescan folders every n seconds; 0 = never
    static constexpr const char* kTheme = "ui.theme";                 // system | light | dark
    static constexpr const char* kViewMode = "ui.view_mode";          // cards | rows
    static constexpr const char* kSortOrder = "ui.sort_order";        // newest | oldest | longest

    static constexpr int kMaxThreads = 64;
    static constexpr int kMaxBeam = 10;
    static constexpr int kDefaultScanIntervalS = 60;

    explicit Settings(Database& db) : m_db(db) {}

    QString value(const QString& key, const QString& fallback = QString()) const;
    bool contains(const QString& key) const;
    bool set(const QString& key, const QString& value);
    bool remove(const QString& key);

    int intValue(const QString& key, int fallback, int minimum, int maximum) const;   // out-of-range / garbage -> fallback
    bool boolValue(const QString& key, bool fallback) const;
    bool setInt(const QString& key, int v) { return set(key, QString::number(v)); }
    bool setBool(const QString& key, bool v) { return set(key, v ? "1" : "0"); }

    // typed views
    AnalysisConfig analysisConfig() const;
    bool autoAnalyze() const { return boolValue(kAutoAnalyze, true); }
    bool watchFolders() const { return boolValue(kScanWatch, true) && scanIntervalSeconds() > 0; }   // an older "interval 0" means off
    int scanIntervalSeconds() const { return intValue(kScanInterval, kDefaultScanIntervalS, 0, 24 * 3600); }
    QString theme() const;       // always one of system|light|dark
    QString viewMode() const;    // cards|rows
    QString sortOrder() const;   // newest|oldest|longest

    struct ModelInfo { QString path; QString name; qint64 sizeBytes = 0; };
    // ggml-*.bin files in `dir`, smallest first (the choice list of the settings dialog)
    static QList<ModelInfo> installedModels(const QString& dir);
    static QString modelDirectory();   // <AppData>/models

private:
    Database& m_db;
};

} // namespace ake
