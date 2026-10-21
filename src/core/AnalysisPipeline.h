#pragma once
#include <atomic>
#include <functional>
#include <memory>

#include "core/IDecoder.h"
#include "core/ITranscriber.h"

namespace ake {

struct AnalysisConfig {
    QString modelPath;             // ggml whisper model; empty = default location
    // Japanese by default: the automatic detection (first 30 s) took a Japanese interview for Norwegian Nynorsk and wrote Latin
    // letters. "auto" stays available in the settings for other languages.
    QString language = QString::fromLatin1(kDefaultLanguage);
    static constexpr const char* kDefaultLanguage = "ja";
    int threads = 0;               // 0 = automatic
    // Beam search width; 0 = greedy. Measured on a 20 min Japanese recording with the small model: greedy fell into a
    // repetition loop for 11.5 min, width 5 not at all (1.5x the time); width 8 looped again, so don't just raise it.
    // Skip whisper for recordings with no speech-like audio (noise, silence) and drop segments it wrote in such stretches.
    // Whisper is confident about words it hears in white noise ("ご視聴ありがとうございました"). See SpeechGate.
    bool speechGate = true;
    int beamSize = kDefaultBeamSize;
    // Forbid whisper's non-speech tokens ("(音声)", "[音楽]"). On a recording from an IC recorder small answered 93% of the
    // time with "(音声)" although people were talking; with the tokens forbidden it wrote the words (which of two near-identical
    // inputs collapsed was a matter of luck: -10 dB bad, -7 good, -5.4 bad). See design.md.
    bool suppressNst = true;
    static constexpr int kDefaultBeamSize = 5;

    static QString defaultModelPath(); // <AppData>/models/ggml-small.bin
};

// The compute part of analysing one recording: decode -> transcribe. No database, no threads of its own, so it is
// testable with fakes. One instance belongs to one worker thread (it caches the loaded model).
class AnalysisPipeline {
public:
    using TranscriberFactory = std::function<std::unique_ptr<ITranscriber>()>;
    using StageProgress = std::function<void(const QString& stage, double overall)>; // overall 0..1

    // Stage names reported to `progress`.
    static const char* const kStageDecode;
    static const char* const kStageLoad;
    static const char* const kStageTranscribe;

    AnalysisPipeline(IDecoder& decoder, TranscriberFactory makeTranscriber);

    // On failure *error is a human readable message; "cancelled" when `cancel` became true.
    bool run(const QString& audioPath, const AnalysisConfig& cfg, const std::atomic<bool>& cancel,
             const StageProgress& progress, TranscriptResult* out, AudioInfo* info, QString* error);

private:
    IDecoder& m_decoder;
    TranscriberFactory m_factory;
    std::unique_ptr<ITranscriber> m_transcriber;
    QString m_loadedModel;
};

} // namespace ake
