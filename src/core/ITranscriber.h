#pragma once
#include <QList>
#include <QString>
#include <optional>
#include <vector>

#include "core/IDecoder.h"

namespace ake {

struct TranscriptSegment {
    qint64 startMs = 0;
    qint64 endMs = 0;
    QString text;
    float noSpeechProb = 0;     // whisper's own estimate that this stretch is not speech (0..1); 0 when unknown
};

struct TranscribeOptions {
    QString language = "auto"; // "auto", "ja", "en", ...
    int threads = 0;           // 0 = automatic
    // Decoder tuning against repetition loops; unset = whisper's default. Beam search is slower but loops less.
    int beamSize = 0;                       // 0 = greedy
    std::optional<float> entropyThold;      // retry hotter when the text compresses better than this (default 2.4)
    std::optional<float> logprobThold;      // ... or when the average log-probability is below this (default -1.0)
    std::optional<float> temperatureInc;    // size of each retry step (default 0.2; 0 turns retrying off)
    std::optional<int> maxTextCtx;          // tokens of previous text given to the decoder (default 16384)
};

struct TranscriptResult {
    QString language; // detected or requested, ISO 639-1
    QString model;    // model file base name, e.g. "ggml-small"
    QList<TranscriptSegment> segments;
};

// Speech-to-text backend. One instance is used by one job at a time.
class ITranscriber {
public:
    virtual ~ITranscriber() = default;

    virtual bool load(const QString& modelPath, QString* error = nullptr) = 0;
    virtual bool isLoaded() const = 0;
    virtual QString modelName() const = 0;

    // `pcm` is 16 kHz mono float (see IDecoder). `progress` gets 0..1; returning false cancels
    // (transcribe() then returns false with *error == "cancelled").
    virtual bool transcribe(const std::vector<float>& pcm, const TranscribeOptions& options, TranscriptResult* out,
                            QString* error = nullptr, const IDecoder::Progress& progress = {}) = 0;
};

} // namespace ake
