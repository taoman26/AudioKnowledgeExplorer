#pragma once
#include <QList>
#include <QString>
#include <functional>

#include "core/ITokenizer.h"

namespace ake {

struct Keyword {
    QString term;   // display form ("Docker Compose", "共有フォルダ")
    QString key;    // normalized (lower-case NFKC), unique per recording
    QString wakati; // space separated tokens, as indexed in FTS (phrase-queryable)
    double score = 0;
    int count = 0;  // occurrences in this recording
};

// How common a term is in the rest of the library (other recordings); drives the IDF part of the score.
struct CorpusStats {
    int otherDocuments = 0;                              // recordings other than the one being analysed
    std::function<int(const QString& wakati)> docFreq;   // of those, how many contain this phrase
};

// Keywords = maximal runs of content nouns ("共有 フォルダ" -> 共有フォルダ), scored by TF x IDF.
class KeywordExtractor {
public:
    static constexpr int kDefaultTopK = 8;
    static constexpr int kMaxRunTokens = 4;   // longer noun runs are lists, not compounds: split into single nouns

    // `tokens` are the transcript tokens in order; pass a Punct token between segments/sentences so runs never
    // span them (use sentenceBreak()).
    static QList<Keyword> extract(const QList<Token>& tokens, const CorpusStats& stats, int topK = kDefaultTopK);

    static Token sentenceBreak() { return Token{ QStringLiteral("\n"), QStringLiteral("\n"), Token::Kind::Punct, false }; }
    static bool isStopword(const QString& lowerKey);
};

} // namespace ake
