#pragma once
#include <QList>
#include <vector>

namespace ake {

struct TranscriptSegment;

// Decides, before whisper runs, which stretches of a recording can hold speech. Whisper "hears" words in broadband noise
// and silence (typically "ご視聴ありがとうございました") and is confident about it, so its own no-speech probability does not
// help. Speech is tonal (low spectral flatness in 100-4000 Hz); hiss, fans and white noise are flat.
//
// Measured on four real 10-minute recordings and a white-noise file (design.md "ノイズへの幻覚"): flatness of the loud
// windows of speech p95 <= 0.15, of white noise 0.30-0.36. Tonal sounds (music, a hum) are NOT recognised as non-speech.
class SpeechGate {
public:
    static constexpr int kSampleRate = 16000;
    static constexpr int kFrame = 512;                 // 32 ms
    static constexpr int kFramesPerWindow = 16;        // 0.512 s windows
    static constexpr float kQuietDb = -60.f;           // below this a window is silence
    static constexpr float kMaxFlatness = 0.25f;       // at or above this a window is noise-like
    static constexpr double kMinSpeechRatio = 0.02;    // a recording with less speech-like audio than this has no speech
    static constexpr double kMinCoverage = 0.3;        // a segment whose span is less speech-like than this is dropped

    struct Result {
        std::vector<bool> speech;                      // per window: could hold speech
        double speechRatio = 0;                        // share of windows that could
        bool hasSpeech() const { return speech.empty() || speechRatio >= kMinSpeechRatio; }   // too short to judge: let it through
        static double windowSeconds() { return double(kFrame) * kFramesPerWindow / kSampleRate; }
        // share of [startMs, endMs) covered by speech-like windows (1 when the span is outside the analysed audio)
        double coverage(long long startMs, long long endMs) const;
    };

    // 16 kHz mono float
    static Result analyze(const std::vector<float>& pcm);
    // Segments that lie in noise-like or silent stretches are removed; returns how many.
    static int dropNonSpeech(const Result& r, QList<TranscriptSegment>* segments);
};

} // namespace ake
