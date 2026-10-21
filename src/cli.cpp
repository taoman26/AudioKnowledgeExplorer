// Headless front-end for the core library: used for scripted verification and as a debugging aid.
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QDateTime>
#include <QElapsedTimer>
#include <cmath>
#include <QSqlQuery>
#include <QTextStream>

#include "core/Database.h"
#include "core/DemoData.h"
#include "core/FFmpegDecoder.h"
#include "core/ITokenizer.h"
#include "platform/QtAudioPlayer.h"
#include <QTimer>
#include "core/JobScheduler.h"
#include "core/KnowledgeBuilder.h"
#include "core/SearchService.h"
#include "core/SpeechGate.h"
#include "core/TextIndexer.h"
#include "core/TranscriptRepository.h"
#include "core/WhisperTranscriber.h"
#include "core/Library.h"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout), err(stderr);

    QCommandLineParser p;
    p.setApplicationDescription("AudioKnowledgeExplorer CLI\n"
                                "commands: add-folder <dir> | list-folders | remove-folder <id> | scan [id] | list | seed-demo | probe <file> | decode <file> | transcribe <file>");
    p.addHelpOption();
    p.addOption({ "db", "Database file (required).", "file" });
    p.addOption({ "no-recursive", "add-folder: do not descend into subfolders." });
    p.addOption({ "exclude", "add-folder: comma separated globs.", "globs" });
    p.addOption({ "model", "transcribe: ggml whisper model file.", "file" });
    p.addOption({ "lang", "transcribe: language (auto, ja, en, ...). Default auto.", "code", "auto" });
    p.addOption({ "max-seconds", "transcribe: only the first N seconds.", "n" });
    p.addOption({ "suppress-nst", "transcribe: forbid non-speech tokens like (音声)." });
    p.addOption({ "threads", "transcribe: thread count (default auto).", "n" });
    p.addOption({ "probs", "transcribe: print whisper's no-speech probability of every segment." });
    p.addOption({ "beam", "transcribe: beam search width (default greedy).", "n" });
    p.addOption({ "entropy", "transcribe: entropy (compression) threshold for retrying hotter.", "x" });
    p.addOption({ "logprob", "transcribe: average log-probability threshold for retrying hotter.", "x" });
    p.addOption({ "temp-inc", "transcribe: temperature step of the retries (0 = no retries).", "x" });
    p.addOption({ "max-ctx", "transcribe: tokens of previous text given to the decoder.", "n" });
    p.addOption({ "save", "transcribe: store the result for this recording id.", "id" });
    p.addOption({ "from", "play: start position in ms.", "ms", "0" });
    p.addOption({ "seconds", "play: stop after this many seconds (default: to the end).", "s", "0" });
    p.addOption({ "volume", "play: 0..1 (default 0.3).", "v", "0.3" });
    p.addOption({ "rate", "play: playback rate (default 1.0).", "r", "1.0" });
    p.addOption({ "rate-reset-after", "play: set the rate back to 1.0 after this many ms (experiment).", "ms", "0" });
    p.addOption({ "rate-after", "play: set --rate only after this many ms of playback.", "ms", "0" });
    p.addOption({ "show", "transcribe: print at most N segments (default 8, 0 = all).", "n", "8" });
    p.addPositionalArgument("command", "command to run");
    p.addPositionalArgument("args", "command arguments", "[args...]");
    p.process(app);

    const QStringList pos = p.positionalArguments();
    if (!p.isSet("db") || pos.isEmpty()) { err << p.helpText(); return 2; }

    ake::Database db;
    QString e;
    if (!db.open(p.value("db"), &e)) { err << "db error: " << e << "\n"; return 1; }
    ake::FFmpegDecoder decoder;
    ake::Library lib(db);
    lib.setDecoder(&decoder);
    const QString cmd = pos[0];

    if (cmd == "add-folder" && pos.size() == 2) {
        const qint64 id = lib.addFolder(pos[1], !p.isSet("no-recursive"), p.value("exclude"), &e);
        if (!id) { err << "error: " << e << "\n"; return 1; }
        out << "added folder " << id << "\n";
    } else if (cmd == "list-folders") {
        for (const auto& f : lib.folders())
            out << f.id << "\t" << f.path << "\trecursive=" << f.recursive << "\texclude=" << f.excludeGlob << "\n";
    } else if (cmd == "remove-folder" && pos.size() == 2) {
        if (!lib.removeFolder(pos[1].toLongLong())) { err << "error: no such folder\n"; return 1; }
        out << "removed\n";
    } else if (cmd == "scan") {
        int rc = 0;
        for (const auto& f : lib.folders()) {
            if (pos.size() == 2 && pos[1].toLongLong() != f.id) continue;
            const ake::ScanResult r = lib.scanFolder(f.id);
            if (!r.ok()) { err << "folder " << f.id << ": " << r.error << "\n"; rc = 1; continue; }
            out << "folder " << f.id << ": added=" << r.added << " updated=" << r.updated << " moved=" << r.moved
                << " removed=" << r.removed << " unchanged=" << r.unchanged << "\n";
        }
        return rc;
    } else if (cmd == "gate" && pos.size() >= 2) {   // gate <file> [recordingId]: what the speech gate makes of a file (and of its saved transcript)
        ake::FFmpegDecoder dec;
        std::vector<float> pcm; QString e;
        if (!dec.decode(pos[1], &pcm, nullptr, &e)) { err << "decode error: " << e << "\n"; return 1; }
        QElapsedTimer t; t.start();
        const ake::SpeechGate::Result g = ake::SpeechGate::analyze(pcm);
        out << "windows=" << g.speech.size() << " speech-like=" << int(g.speechRatio * 100) << "% hasSpeech=" << (g.hasSpeech() ? "yes" : "NO")
            << " analysed in " << t.elapsed() << " ms\n";
        if (pos.size() >= 3) {
            ake::TranscriptResult tr;
            if (ake::TranscriptRepository(db).loadCurrent(pos[2].toLongLong(), &tr)) {
                QList<ake::TranscriptSegment> segs = tr.segments;
                const int total = int(segs.size());
                QList<ake::TranscriptSegment> kept = segs;
                const int dropped = ake::SpeechGate::dropNonSpeech(g, &kept);
                out << "saved transcript: " << total << " segments, the gate would drop " << dropped << "\n";
                for (const auto& sg : segs) {
                    bool k = false; for (const auto& x : kept) k |= x.startMs == sg.startMs && x.text == sg.text;
                    if (!k) out << "  dropped [" << sg.startMs / 1000 << "s] " << sg.text << "\n";
                }
            }
        }
    } else if (cmd == "probe" && pos.size() == 2) {
        ake::AudioInfo i;
        if (!decoder.probe(pos[1], &i, &e)) { err << "error: " << e << "\n"; return 1; }
        out << "codec=" << i.codec << " rate=" << i.sampleRate << " channels=" << i.channels << " duration_ms=" << i.durationMs
            << " title=" << i.title << " creation_time=" << (i.creationTime.isValid() ? i.creationTime.toString(Qt::ISODate) : "-") << "\n";
    } else if (cmd == "decode" && pos.size() == 2) {
        std::vector<float> pcm;
        ake::AudioInfo i;
        QElapsedTimer t; t.start();
        if (!decoder.decode(pos[1], &pcm, &i, &e)) { err << "error: " << e << "\n"; return 1; }
        float peak = 0; double sq = 0;
        for (float x : pcm) { peak = qMax(peak, qAbs(x)); sq += double(x) * x; }
        out << "samples=" << qulonglong(pcm.size()) << " seconds=" << pcm.size() / double(ake::kDecodeSampleRate)
            << " peak=" << peak << " rms=" << (pcm.empty() ? 0.0 : std::sqrt(sq / pcm.size())) << " took_ms=" << t.elapsed() << "\n";
    } else if (cmd == "transcribe" && pos.size() == 2) {
        if (!p.isSet("model")) { err << "error: --model is required\n"; return 2; }
        std::vector<float> pcm;
        ake::AudioInfo i;
        if (!decoder.decode(pos[1], &pcm, &i, &e)) { err << "decode error: " << e << "\n"; return 1; }
        if (p.isSet("max-seconds"))
            pcm.resize(qMin<size_t>(pcm.size(), size_t(p.value("max-seconds").toDouble() * ake::kDecodeSampleRate)));
        ake::WhisperTranscriber whisper;
        QElapsedTimer load; load.start();
        if (!whisper.load(p.value("model"), &e)) { err << "model error: " << e << "\n"; return 1; }
        const qint64 loadMs = load.elapsed();
        ake::TranscribeOptions o;
        o.language = p.value("lang");
        o.threads = p.value("threads").toInt();
        o.beamSize = p.value("beam").toInt();
        if (p.isSet("entropy")) o.entropyThold = p.value("entropy").toFloat();
        if (p.isSet("logprob")) o.logprobThold = p.value("logprob").toFloat();
        if (p.isSet("temp-inc")) o.temperatureInc = p.value("temp-inc").toFloat();
        if (p.isSet("suppress-nst")) o.suppressNst = true;
        if (p.isSet("max-ctx")) o.maxTextCtx = p.value("max-ctx").toInt();
        ake::TranscriptResult r;
        QElapsedTimer t; t.start();
        int lastPct = -10;
        const bool ok = whisper.transcribe(pcm, o, &r, &e, [&](double f) {
            const int pct = int(f * 100);
            if (pct >= lastPct + 10) { lastPct = pct; err << "  progress " << pct << "%\n"; err.flush(); }
            return true;
        });
        if (!ok) { err << "transcribe error: " << e << "\n"; return 1; }
        const double audioSec = pcm.size() / double(ake::kDecodeSampleRate), took = t.elapsed() / 1000.0;
        out << "model=" << r.model << " language=" << r.language << " segments=" << r.segments.size()
            << " audio_s=" << audioSec << " load_ms=" << loadMs << " took_s=" << took << " realtime_factor=" << (audioSec > 0 ? took / audioSec : 0) << "\n";
        auto stamp = [](qint64 ms) {
            return QString("%1:%2.%3").arg(ms / 60000, 2, 10, QChar('0')).arg(ms / 1000 % 60, 2, 10, QChar('0')).arg(ms % 1000, 3, 10, QChar('0'));
        };
        int limit = p.value("show").toInt();
        int n = 0;
        for (const auto& s : r.segments) {
            if (limit && n++ >= limit) { out << "... (" << r.segments.size() - limit << " more)\n"; break; }
            out << QString("[%1 - %2] %3%4\n").arg(stamp(s.startMs), stamp(s.endMs), s.text, p.isSet("probs") ? QString("  {nsp=%1}").arg(s.noSpeechProb, 0, 'f', 2) : QString());
        }
        if (p.isSet("save")) {
            ake::TranscriptRepository repo(db);
            const qint64 tid = repo.save(p.value("save").toLongLong(), r, &e);
            if (!tid) { err << "save error: " << e << "\n"; return 1; }
            out << "saved transcript " << tid << " for recording " << p.value("save") << "\n";
        }
    } else if (cmd == "analyze") {
        ake::JobScheduler scheduler(db, decoder, [] { return std::unique_ptr<ake::ITranscriber>(new ake::WhisperTranscriber); });
        scheduler.setConfig({ p.value("model"), p.value("lang"), p.value("threads").toInt() });
        const auto tokenizer = ake::createTokenizer();
        scheduler.setTokenizer(tokenizer.get());
        QObject::connect(&scheduler, &ake::JobScheduler::recordingChanged, [&](qint64 id) {
            QSqlQuery q(db.handle());
            q.exec(QString("SELECT status, error_msg FROM recordings WHERE id=%1").arg(id));
            if (q.next()) out << "recording " << id << " -> " << q.value(0).toString()
                              << (q.value(1).toString().isEmpty() ? "" : " (" + q.value(1).toString() + ")") << "\n";
            out.flush();
        });
        int lastPct = -10;
        QObject::connect(&scheduler, &ake::JobScheduler::progressChanged, [&](qint64 id, const QString& stage, double v) {
            const int pct = int(v * 100);
            if (pct >= lastPct + 10 || pct < lastPct) { lastPct = pct; out << "  #" << id << " " << stage << " " << pct << "%\n"; out.flush(); }
        });
        int queued = 0;
        if (pos.size() == 1 || (pos.size() == 2 && pos[1] == "--all")) queued = scheduler.enqueueAllNew();
        else for (int i = 1; i < pos.size(); ++i) queued += scheduler.enqueue(pos[i].toLongLong());
        out << "queued " << queued << " recording(s)\n"; out.flush();
        QElapsedTimer t; t.start();
        scheduler.waitUntilIdle();
        out << "done in " << t.elapsed() / 1000.0 << " s\n";
    } else if (cmd == "play" && pos.size() == 2) {
        ake::QtAudioPlayer player;
        player.setVolume(p.value("volume").toDouble());
        const double wantedRate = p.value("rate").toDouble();
        if (p.value("rate-after").toInt() == 0) player.setRate(wantedRate);
        else QTimer::singleShot(p.value("rate-after").toInt(), [&] { player.setRate(wantedRate); out << "rate set to " << wantedRate << " at position " << player.position() << "\n"; out.flush(); });
        if (p.value("rate-reset-after").toInt() > 0)
            QTimer::singleShot(p.value("rate-reset-after").toInt(), [&] { player.setRate(1.0); out << "rate reset to 1.0 at position " << player.position() << "\n"; out.flush(); });
        int rc = 0;
        qint64 maxPos = 0;
        QObject::connect(&player, &ake::IAudioPlayer::errorOccurred, [&](const QString& m) { err << "error: " << m << "\n"; err.flush(); rc = 1; QCoreApplication::quit(); });
        QObject::connect(&player, &ake::IAudioPlayer::stateChanged, [&](ake::IAudioPlayer::State s) { out << "state " << int(s) << "\n"; out.flush(); });
        QObject::connect(&player, &ake::IAudioPlayer::durationChanged, [&](qint64 d) { out << "duration " << d << " ms\n"; out.flush(); });
        QObject::connect(&player, &ake::IAudioPlayer::positionChanged, [&](qint64 ms) { maxPos = qMax(maxPos, ms); });
        QObject::connect(&player, &ake::IAudioPlayer::mediaEnded, [&] { out << "ended\n"; out.flush(); QCoreApplication::quit(); });
        player.load(pos[1]);
        if (p.value("from").toLongLong() > 0) player.seek(p.value("from").toLongLong());
        player.play();
        if (p.value("seconds").toDouble() > 0)
            QTimer::singleShot(int(p.value("seconds").toDouble() * 1000), [&] { QCoreApplication::quit(); });
        QElapsedTimer t; t.start();
        QCoreApplication::exec();
        out << "played " << t.elapsed() << " ms wall, furthest position " << maxPos << " ms\n";
        return rc;
    } else if (cmd == "index" && pos.size() == 2) {
        const auto tokenizer = ake::createTokenizer();
        ake::TextIndexer::Result r;
        if (!ake::TextIndexer(db, *tokenizer).index(pos[1].toLongLong(), &r, &e)) { err << "error: " << e << "\n"; return 1; }
        out << "tokenizer=" << tokenizer->name() << " segments=" << r.segments << " keywords=" << r.keywords.size() << "\n";
        for (const auto& k : r.keywords) out << QString("  %1\t%2x\tscore=%3").arg(k.term, -22).arg(k.count).arg(k.score, 0, 'f', 2) << "\n";
    } else if (cmd == "build" && pos.size() == 2) {
        const auto tokenizer = ake::createTokenizer();
        ake::KnowledgeBuilder::Result r;
        QElapsedTimer t; t.start();
        if (!ake::KnowledgeBuilder(db, *tokenizer).build(pos[1].toLongLong(), &r, &e)) { err << "error: " << e << "\n"; return 1; }
        out << "segments=" << r.segments << " keywords=" << r.keywords.size() << " took_ms=" << t.elapsed() << "\n";
        out << "title:   " << (r.title.isEmpty() ? "(none: file name is used)" : r.title) << (r.keptUserTitle ? "  [kept the user's title]" : "") << "\n";
        out << "summary: " << (r.summary.isEmpty() ? "(none)" : r.summary) << (r.keptUserSummary ? "  [kept the user's summary]" : "") << "\n";
        for (const auto& k : r.keywords) out << "  kw " << k.term << " (" << k.count << "x)\n";
    } else if (cmd == "search" && pos.size() >= 2) {
        const auto tokenizer = ake::createTokenizer();
        ake::SearchService svc(db, *tokenizer);
        QElapsedTimer t; t.start();
        const auto r = svc.search(pos.mid(1).join(' '));
        out << r.hits.size() << " recording(s), " << r.totalSegmentHits << " segment hit(s), " << t.elapsed() << " ms; terms: " << r.terms.join(" | ") << "\n";
        for (const auto& h : r.hits) {
            QString s = h.snippet;
            for (int i = int(h.highlights.size()) - 2; i >= 0; i -= 2) { s.insert(h.highlights[i] + h.highlights[i + 1], "]"); s.insert(h.highlights[i], "["); }
            static const char* src[] = { "transcript", "summary", "title", "filename" };
            out << QString("  #%1 score=%2 hits=%3 %4 @%5ms  %6").arg(h.recordingId).arg(h.score, 0, 'f', 2).arg(h.segmentHits)
                       .arg(src[int(h.source)]).arg(h.snippetMs).arg(s) << "\n";
        }
    } else if (cmd == "keywords") {
        QSqlQuery q(db.handle());
        q.exec(QString("SELECT rk.recording_id, k.name, rk.score, rk.source FROM recording_keywords rk JOIN keywords k ON k.id=rk.keyword_id%1"
                       " ORDER BY rk.recording_id, rk.score DESC").arg(pos.size() == 2 ? " WHERE rk.recording_id=" + QString::number(pos[1].toLongLong()) : ""));
        while (q.next()) out << q.value(0).toString() << "\t" << q.value(1).toString() << "\t" << q.value(2).toDouble() << "\t" << q.value(3).toString() << "\n";
    } else if (cmd == "tokenize" && pos.size() >= 2) {
        QString note;
        auto tok = ake::createTokenizer(&note);
        if (!note.isEmpty()) err << "note: " << note << "\n";
        const QString text = pos.mid(1).join(' ');
        static const char* kinds[] = { "Noun", "Number", "Verb", "Adj", "Other", "Punct" };
        out << "tokenizer=" << tok->name() << "\n";
        for (const auto& t : tok->tokenize(text))
            out << t.surface << "\t" << kinds[int(t.kind)] << (t.weak ? "(weak)" : "") << "\t" << t.base << "\n";
        out << "wakati: " << tok->wakati(text) << "\n";
        out << "phonetic: " << tok->phonetic(text) << "\n";
    } else if (cmd == "phonetic" && pos.size() >= 2) {   // phonetic <file>: the phonetic form of every line (experiments)
        auto tok = ake::createTokenizer();
        QFile f(pos[1]);
        if (!f.open(QIODevice::ReadOnly)) { err << "cannot read " << pos[1] << "\n"; return 1; }
        for (const QByteArray& l : f.readAll().split('\n')) out << tok->phonetic(QString::fromUtf8(l)) << "\n";
    } else if (cmd == "seed-demo") {
        if (!ake::seedDemoData(db)) { err << "seed failed\n"; return 1; }
        out << "seeded demo data (fake recordings)\n";
    } else if (cmd == "list") {
        QSqlQuery q(db.handle());
        q.exec("SELECT id,format,status,duration_ms,rel_path FROM recordings ORDER BY rel_path");
        while (q.next())
            out << q.value(0).toString() << "\t" << q.value(1).toString() << "\t" << q.value(2).toString() << "\t"
                << q.value(3).toString() << "\t" << q.value(4).toString() << "\n";
        out << lib.recordingCount() << " recording(s)\n";
    } else {
        err << p.helpText();
        return 2;
    }
    return 0;
}
