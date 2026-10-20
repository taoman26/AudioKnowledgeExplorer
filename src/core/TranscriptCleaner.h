#pragma once
#include <QList>
#include <QStringList>

namespace ake {

// Whisper sometimes gets stuck and repeats a phrase (as many identical segments, or one segment containing the same
// phrase over and over, or a cycle of a few different segments). Counting or summarizing that text would put the hallucination on the card, so keywords and
// summaries are computed from the cleaned text. The search index keeps the original wording.
class TranscriptCleaner {
public:
    // Drops a segment identical to the previous kept one (or, when long enough to be a phrase, to one of the 3 before),
    // and collapses in-segment loops ("ABCABCABC" -> "ABC"). Order is preserved; empty segments disappear.
    static QStringList clean(const QStringList& segments);
    static QString collapseLoops(const QString& text);
    // Whisper writes silence and sounds as bracketed tags ("[音声なし]", "[音楽]", "[BLANK_AUDIO]"): not speech, so not summary or
    // snippet material. Removes short square-bracket tags; the stored transcript is untouched.
    static QString stripNonSpeechTags(const QString& text);
    // Which segments (by fingerprint, same length as the input) belong to the repeats of a multi-segment cycle.
    static QList<bool> loopMask(const QStringList& fingerprints);
    static constexpr int kMaxCycle = 4;    // longest cycle of segments that is recognised
    static constexpr int kMinCycles = 3;   // rounds in a row before it counts as a loop
};

} // namespace ake
