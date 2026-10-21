#include "core/WhisperTranscriber.h"

#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <atomic>

#include <whisper.h>

namespace ake {

namespace {

void quietLog(enum ggml_log_level level, const char* text, void*)
{
    if (level == GGML_LOG_LEVEL_ERROR)
        qWarning("whisper: %s", text);
}

struct RunState {
    const IDecoder::Progress* progress = nullptr;
    std::atomic<bool> cancelled{ false };
};

} // namespace

WhisperTranscriber::WhisperTranscriber()
{
    static const bool once = [] { whisper_log_set(quietLog, nullptr); return true; }();
    (void)once;
}

WhisperTranscriber::~WhisperTranscriber()
{
    if (m_ctx) whisper_free(m_ctx);
}

int WhisperTranscriber::defaultThreads()
{
#ifdef __HAIKU__
    return qBound(1, QThread::idealThreadCount(), 3); // measured on Haiku (8 CPUs): 3 threads is fastest, 5+ are slower again
#else
    return qBound(1, QThread::idealThreadCount(), 8); // beyond ~8 threads whisper.cpp stops scaling
#endif
}

bool WhisperTranscriber::load(const QString& modelPath, QString* error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ctx) { whisper_free(m_ctx); m_ctx = nullptr; m_modelName.clear(); }
    if (!QFileInfo::exists(modelPath)) {
        if (error) *error = QStringLiteral("model file not found: %1").arg(modelPath);
        return false;
    }
    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = false;
    m_ctx = whisper_init_from_file_with_params(QFile::encodeName(modelPath).constData(), cp);
    if (!m_ctx) {
        if (error) *error = QStringLiteral("cannot load model (not a ggml whisper model?): %1").arg(modelPath);
        return false;
    }
    m_modelName = QFileInfo(modelPath).completeBaseName();
    return true;
}

bool WhisperTranscriber::transcribe(const std::vector<float>& pcm, const TranscribeOptions& options,
                                    TranscriptResult* out, QString* error, const IDecoder::Progress& progress)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    if (!m_ctx) return fail("no model loaded");
    if (pcm.empty()) return fail("no audio");
    *out = {};

    whisper_full_params p = whisper_full_default_params(options.beamSize > 0 ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY);
    if (options.beamSize > 0) p.beam_search.beam_size = options.beamSize;
    p.print_progress = false;
    p.print_realtime = false;
    p.print_timestamps = false;
    p.print_special = false;
    p.translate = false;
    const QByteArray lang = options.language.isEmpty() ? QByteArray("auto") : options.language.toLatin1();
    p.language = lang.constData();
    p.detect_language = false; // "auto" in `language` makes whisper detect it
    if (options.entropyThold) p.entropy_thold = *options.entropyThold;
    if (options.logprobThold) p.logprob_thold = *options.logprobThold;
    if (options.temperatureInc) p.temperature_inc = *options.temperatureInc;
    if (options.suppressNst) p.suppress_nst = *options.suppressNst;
    if (options.maxTextCtx) p.n_max_text_ctx = *options.maxTextCtx;
    p.n_threads = options.threads > 0 ? options.threads : defaultThreads();

    RunState state;
    state.progress = &progress;
    p.progress_callback_user_data = &state;
    p.progress_callback = [](whisper_context*, whisper_state*, int pct, void* user) {
        auto* s = static_cast<RunState*>(user);
        if (s->progress && *s->progress && !(*s->progress)(pct / 100.0)) s->cancelled = true;
    };
    p.abort_callback_user_data = &state;
    p.abort_callback = [](void* user) { return static_cast<RunState*>(user)->cancelled.load(); };

    const int rc = whisper_full(m_ctx, p, pcm.data(), int(pcm.size()));
    if (state.cancelled) return fail("cancelled");
    if (rc != 0) return fail(QStringLiteral("whisper_full failed (code %1)").arg(rc));

    out->model = m_modelName;
    QStringList used = { options.beamSize > 0 ? QStringLiteral("beam=%1").arg(options.beamSize) : QStringLiteral("greedy"),
                         QStringLiteral("threads=%1").arg(p.n_threads), QStringLiteral("lang=%1").arg(QString::fromLatin1(lang)) };
    if (options.entropyThold) used << QStringLiteral("entropy=%1").arg(*options.entropyThold);
    if (options.logprobThold) used << QStringLiteral("logprob=%1").arg(*options.logprobThold);
    if (options.temperatureInc) used << QStringLiteral("temp-inc=%1").arg(*options.temperatureInc);
    if (options.suppressNst && *options.suppressNst) used << QStringLiteral("suppress-nst");
    if (options.maxTextCtx) used << QStringLiteral("max-ctx=%1").arg(*options.maxTextCtx);
    out->settings = used.join(' ');
    out->language = QString::fromLatin1(whisper_lang_str(whisper_full_lang_id(m_ctx)));
    const int n = whisper_full_n_segments(m_ctx);
    for (int i = 0; i < n; ++i) {
        const QString text = QString::fromUtf8(whisper_full_get_segment_text(m_ctx, i)).trimmed();
        if (text.isEmpty()) continue;
        // whisper timestamps are in units of 10 ms
        out->segments.append({ whisper_full_get_segment_t0(m_ctx, i) * 10, whisper_full_get_segment_t1(m_ctx, i) * 10, text,
                                whisper_full_get_segment_no_speech_prob(m_ctx, i) });
    }
    if (progress) progress(1.0);
    return true;
}

} // namespace ake
