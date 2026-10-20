#pragma once
#include "core/ISummarizer.h"

namespace ake {

// TextRank over sentences: similarity = cosine of TF-IDF vectors of content nouns; the top sentences, adjusted for
// topic keywords, length and position, are chosen greedily (skipping near-duplicates) and listed chronologically.
class ExtractiveSummarizer : public ISummarizer {
public:
    QString name() const override { return QStringLiteral("extractive-textrank-v1"); }
    QString summarize(const SummaryInput& in, const SummaryOptions& opt = {}) const override;

    // Joins the segments into sentences: breaks after 。！？ (and ". " in Latin text), never inside decimals, and at a
    // segment boundary once a sentence has grown past `forceBreakChars` (speech without punctuation: beam search and
    // large models print almost none). Such a forced sentence keeps its segment boundaries as "、" and ends with "。",
    // so the summary reads as sentences instead of one run-on; sentences that end by themselves are joined as is.
    static QStringList splitSentences(const QStringList& segments, int forceBreakChars = kForceBreakChars);
    static constexpr int kForceBreakChars = 70;
    static constexpr int kMinPieceForComma = 5;   // segments shorter than this are not followed by a comma

    static constexpr int kMinChars = 12;        // shorter sentences are not summary material
    static constexpr int kMaxChars = 200;
    static constexpr int kMaxCandidates = 1200; // TextRank is O(n^2): longer transcripts are pre-filtered
};

} // namespace ake
