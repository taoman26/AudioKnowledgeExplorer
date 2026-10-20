#include "core/AnalysisPipeline.h"

#include <QFileInfo>

#include "core/AppPaths.h"
#include "core/SpeechGate.h"

#include <QStandardPaths>

namespace ake {

const char* const AnalysisPipeline::kStageDecode = "decode";
const char* const AnalysisPipeline::kStageLoad = "load";
const char* const AnalysisPipeline::kStageTranscribe = "transcribe";

namespace {
// Share of the overall bar per stage: decoding is quick, whisper dominates.
constexpr double kDecodeShare = 0.05;
constexpr double kLoadShare = 0.05;
}

QString AnalysisConfig::defaultModelPath()
{
    return AppPaths::modelDirectory() + "/ggml-small.bin";
}

AnalysisPipeline::AnalysisPipeline(IDecoder& decoder, TranscriberFactory makeTranscriber)
    : m_decoder(decoder), m_factory(std::move(makeTranscriber))
{
}

bool AnalysisPipeline::run(const QString& audioPath, const AnalysisConfig& cfg, const std::atomic<bool>& cancel,
                           const StageProgress& progress, TranscriptResult* out, AudioInfo* info, QString* error)
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    auto report = [&](const char* stage, double v) { if (progress) progress(QString::fromLatin1(stage), v); };

    report(kStageDecode, 0);
    std::vector<float> pcm;
    QString err;
    const bool decoded = m_decoder.decode(audioPath, &pcm, info, &err, [&](double f) {
        if (f >= 0) report(kStageDecode, f * kDecodeShare);
        return !cancel.load();
    });
    if (!decoded) return fail(err.isEmpty() ? "decode failed" : err);
    if (cancel.load()) return fail("cancelled");

    const QString model = cfg.modelPath.isEmpty() ? AnalysisConfig::defaultModelPath() : cfg.modelPath;
    SpeechGate::Result gate;
    if (cfg.speechGate) {
        gate = SpeechGate::analyze(pcm);
        if (!gate.hasSpeech()) {                       // nothing but noise / silence: an empty transcript, no model needed
            *out = {};
            out->model = QFileInfo(model).completeBaseName();
            out->language = cfg.language == "auto" ? QString() : cfg.language;
            report(kStageTranscribe, 1.0);
            return true;
        }
    }
    if (cancel.load()) return fail("cancelled");
    if (!m_transcriber || m_loadedModel != model) {
        report(kStageLoad, kDecodeShare);
        m_transcriber = m_factory();
        m_loadedModel.clear();
        if (!m_transcriber->load(model, &err)) { m_transcriber.reset(); return fail(err); }
        m_loadedModel = model;
    }
    if (cancel.load()) return fail("cancelled");

    const double base = kDecodeShare + kLoadShare;
    report(kStageTranscribe, base);
    TranscribeOptions opt;
    opt.language = cfg.language;
    opt.threads = cfg.threads;
    opt.beamSize = cfg.beamSize;
    const bool ok = m_transcriber->transcribe(pcm, opt, out, &err, [&](double f) {
        report(kStageTranscribe, base + f * (1.0 - base));
        return !cancel.load();
    });
    if (!ok) return fail(err.isEmpty() ? "transcription failed" : err);
    if (cfg.speechGate) SpeechGate::dropNonSpeech(gate, &out->segments);
    report(kStageTranscribe, 1.0);
    return true;
}

} // namespace ake
