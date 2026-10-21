#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QLineEdit>
#include <QStandardPaths>
#include <QTimer>

#include "core/Database.h"
#include "core/FFmpegDecoder.h"
#include "core/ITokenizer.h"
#include <QDialog>
#include "core/AppPaths.h"
#include "core/JobScheduler.h"
#include "core/PollingFolderWatcher.h"
#include "core/ScanService.h"
#include "core/Settings.h"
#include "core/WhisperTranscriber.h"
#include "platform/QtAudioPlayer.h"

#include "ui/CardListView.h"
#include "ui/FolderDialog.h"
#include "ui/MainWindow.h"
#include "ui/SettingsDialog.h"
#include "ui/PlaybackController.h"
#include "ui/RecordingListModel.h"
#include "ui/SidebarWidget.h"
#include "ui/TranscriptView.h"
#include "ui/Theme.h"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("AudioKnowledgeExplorer");

    QCommandLineParser p;
    p.addHelpOption();
    // Debug/verification option: render the window to a PNG and exit (works with QT_QPA_PLATFORM=offscreen).
    QCommandLineOption grab("grab", "Save a screenshot to <file> and exit.", "file");
    p.addOption(grab);
    QCommandLineOption theme("theme", "Force theme: light or dark (default: follow system).", "name");
    p.addOption(theme);
    QCommandLineOption dbOpt("db", "Database file (default: app data dir).", "file");
    p.addOption(dbOpt);
    QCommandLineOption selOpt("select", "Debug: select card at row <n> before grabbing.", "n");
    p.addOption(selOpt);
    QCommandLineOption clickOpt("click", "Debug: activate the sidebar item with this label (repeatable).", "label");
    p.addOption(clickOpt);
    QCommandLineOption searchOpt("search", "Debug: type this into the search box (and run it) before grabbing.", "text");
    p.addOption(searchOpt);
    QCommandLineOption blurOpt("blur", "Debug: take the focus off the search box (closes the suggestions) before grabbing.");
    p.addOption(blurOpt);
    QCommandLineOption openOpt("open-transcript", "Debug: open the full-text screen of the card at row <n>.", "n");
    p.addOption(openOpt);
    QCommandLineOption msOpt("ms", "With --open-transcript: scroll to this time (ms).", "ms", "-1");
    p.addOption(msOpt);
    QCommandLineOption tqOpt("tquery", "With --open-transcript: in-page search text.", "text");
    p.addOption(tqOpt);
    QCommandLineOption playOpt("play", "Debug: start playing the card at row <n> (with --play-ms: from there; volume is kept low).", "n");
    p.addOption(playOpt);
    QCommandLineOption playMsOpt("play-ms", "With --play: start position in ms.", "ms", "0");
    p.addOption(playMsOpt);
    QCommandLineOption listOpt("list", "Start in the dense list view instead of cards.");
    p.addOption(listOpt);
    QCommandLineOption sortOpt("sort", "Initial order: newest (default), oldest, longest.", "order", "newest");
    p.addOption(sortOpt);
    QCommandLineOption sizeOpt("size", "Window size as WxH (for screenshots).", "WxH");
    p.addOption(sizeOpt);
    QCommandLineOption modelOpt("model", "whisper ggml model file (default: <app data>/models/ggml-small.bin).", "file");
    p.addOption(modelOpt);
    QCommandLineOption langOpt("lang", "Transcription language: auto, ja, en, ... (debug: overrides the setting for this run).", "code");
    p.addOption(langOpt);
    QCommandLineOption analyzeAll("analyze-all", "Debug: queue every not yet analysed recording at start.");
    p.addOption(analyzeAll);
    QCommandLineOption delayOpt("grab-delay", "With --grab: wait <ms> before the screenshot (default 200).", "ms", "200");
    p.addOption(delayOpt);
    QCommandLineOption dialogOpt("show-dialog", "Debug: open the folders or settings dialog (with --grab: screenshot of the dialog).", "name");
    p.addOption(dialogOpt);
    p.process(app);

    QString dbPath = p.value(dbOpt);
    if (dbPath.isEmpty()) {
        QDir().mkpath(ake::AppPaths::dataDirectory());
        dbPath = ake::AppPaths::defaultDatabasePath();
    }
    ake::Database db;
    QString dbErr;
    if (!db.open(dbPath, &dbErr)) {
        qCritical("cannot open database %s: %s", qPrintable(dbPath), qPrintable(dbErr));
        return 1;
    }

    // Declaration order matters: the window goes first, then the scheduler (joins its worker thread), then the rest.
    ake::FFmpegDecoder decoder;
    ake::JobScheduler scheduler(db, decoder, [] { return std::unique_ptr<ake::ITranscriber>(new ake::WhisperTranscriber); });
    QString tokNote;
    const auto tokenizer = ake::createTokenizer(&tokNote);
    if (!tokNote.isEmpty()) qWarning("%s", qPrintable(tokNote));
    scheduler.setTokenizer(tokenizer.get());
    {   // stored settings; the debug options override them for this run
        ake::AnalysisConfig cfg = ake::Settings(db).analysisConfig();
        if (p.isSet(modelOpt)) cfg.modelPath = p.value(modelOpt);
        if (p.isSet(langOpt)) cfg.language = p.value(langOpt);
        scheduler.setConfig(cfg);
    }

    ake::ScanService scanner(db, &decoder);
    ake::PollingFolderWatcher watcher;
    ake::QtAudioPlayer audio;                 // declared before the window, so it outlives it (the window stops playback in its destructor)
    ake::MainWindow w;
    w.setTokenizer(tokenizer.get());
    w.setDatabase(&db);
    if (p.isSet(listOpt)) w.setViewMode(ake::CardListView::Mode::Rows);
    if (p.value(sortOpt) == "oldest") w.setSortOrder(ake::RecordingListModel::SortOrder::Oldest);
    else if (p.value(sortOpt) == "longest") w.setSortOrder(ake::RecordingListModel::SortOrder::Longest);
    w.setAudioPlayer(&audio);
    w.setScheduler(&scheduler);
    w.setScanService(&scanner);
    QObject::connect(&watcher, &ake::IFolderWatcher::changeSuspected, &scanner, &ake::ScanService::scanAll);
    watcher.start(ake::Settings(db).watchFolders() ? ake::Settings(db).scanIntervalSeconds() : 0);
    QObject::connect(&w, &ake::MainWindow::settingsChanged, [&] { watcher.start(ake::Settings(db).watchFolders() ? ake::Settings(db).scanIntervalSeconds() : 0); });
    scanner.scanAll();                  // pick up changes made while the app was closed
    scheduler.resumeInterrupted(); // jobs left over from the previous run continue
    if (p.isSet(analyzeAll)) scheduler.enqueueAllNew();
    w.applyThemeName(p.isSet(theme) ? p.value(theme) : ake::Settings(db).theme());
    if (p.isSet(sizeOpt)) { const auto wh = p.value(sizeOpt).split('x'); if (wh.size() == 2) w.resize(wh[0].toInt(), wh[1].toInt()); }
    w.show();
    if (p.isSet(searchOpt)) { w.searchBox()->setFocus(); w.searchBox()->setText(p.value(searchOpt)); w.runSearchNow(); if (p.isSet(blurOpt)) w.cardView()->setFocus(); }
    for (const QString& label : p.values(clickOpt)) {
        auto* sb = w.sidebarWidget();
        bool hit = false;
        for (int i = 0; i < sb->topLevelItemCount(); ++i)
            if (sb->topLevelItem(i)->text(0) == label) { sb->itemClicked(sb->topLevelItem(i), 0); hit = true; break; }
        if (!hit) qWarning("--click: no sidebar item '%s'", qPrintable(label));
    }
    if (p.isSet(openOpt) && w.recordingModel()) {
        const QModelIndex idx = w.recordingModel()->index(p.value(openOpt).toInt(), 0);
        w.showTranscript(idx.data(ake::RecordingListModel::IdRole).toLongLong(), p.value(msOpt).toLongLong(), p.value(tqOpt));
    }
    if (p.isSet(playOpt) && w.recordingModel() && w.playback()) {
        audio.setVolume(0.05);
        const QModelIndex idx = w.recordingModel()->index(p.value(playOpt).toInt(), 0);
        w.playback()->play(idx.data(ake::RecordingListModel::IdRole).toLongLong(), p.value(playMsOpt).toLongLong());
    }
    if (p.isSet(selOpt) && w.recordingModel())
        w.cardView()->setCurrentIndex(w.recordingModel()->index(p.value(selOpt).toInt(), 0));

    QWidget* shot = &w;
    if (p.isSet(dialogOpt)) {
        QDialog* dlg = p.value(dialogOpt) == "settings" ? static_cast<QDialog*>(w.createSettingsDialog())
                                                         : static_cast<QDialog*>(w.createFolderDialog());
        if (dlg) { dlg->show(); shot = dlg; }
        else qWarning("--show-dialog: unknown name or no database");
    }
    if (p.isSet(grab)) {
        const QString out = p.value(grab);
        QTimer::singleShot(p.value(delayOpt).toInt(), [&] {
            const bool ok = shot->grab().save(out);
            qInfo("grab %s: %s", qPrintable(out), ok ? "ok" : "FAILED");
            QApplication::exit(ok ? 0 : 1);
        });
    }
    return app.exec();
}
