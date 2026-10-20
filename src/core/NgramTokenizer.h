#pragma once
#include "core/ITokenizer.h"

namespace ake {

// Dictionary-free fallback: ASCII/digit runs become words; kanji/katakana runs become overlapping bigrams;
// hiragana (mostly particles and endings) is dropped as Other. Good enough for search, weak for keywords.
class NgramTokenizer : public ITokenizer {
public:
    QString name() const override { return QStringLiteral("ngram"); }
    QList<Token> tokenize(const QString& text) const override;
};

} // namespace ake
