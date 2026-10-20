#include "core/KeywordExtractor.h"

#include <QHash>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace ake {

namespace {

// Latin words MeCab happily calls nouns, plus very generic Japanese nouns that appear in every talk.
const QSet<QString>& stopwords()
{
    static const QSet<QString> s = [] {
        QSet<QString> w;
        for (const char* x : { "the", "a", "an", "and", "or", "but", "if", "of", "to", "in", "on", "at", "by", "for",
                               "with", "from", "as", "is", "are", "was", "were", "be", "been", "it", "its", "this",
                               "that", "these", "those", "i", "you", "he", "she", "we", "they", "me", "my", "your",
                               "our", "their", "his", "her", "not", "no", "so", "do", "does", "did", "have", "has",
                               "had", "will", "would", "can", "could", "should", "just", "about", "what", "which",
                               "who", "when", "where", "how", "why", "there", "here", "then", "than", "also",
                               "very", "ok", "yeah", "um", "uh", "etc", "vs" })
            w.insert(QString::fromLatin1(x));
        for (const char* x : { "こと", "もの", "ため", "よう", "とき", "ところ", "感じ", "部分", "今回", "前回", "以下",
                               "以上", "これ", "それ", "あれ", "ひと", "人", "方", "中", "的", "等", "話", "今",
                               "自分", "皆さん", "みなさん", "私", "僕", "俺", "あなた", "我々", "弊社", "当社", "御社", "貴社", "皆様", "みんな", "先ほど", "さっき", "今日",
                               "昨日", "明日", "お願い", "ありがとう", "ちょっと", "ほう", "うち" })
            w.insert(QString::fromUtf8(x));
        return w;
    }();
    return s;
}

bool isLatinWord(const QString& s)
{
    for (QChar c : s) if (c.unicode() >= 0x80 || !(c.isLetterOrNumber() || c == '-' || c == '_')) return false;
    return !s.isEmpty();
}

bool allHiragana(const QString& s)
{
    for (QChar c : s) if (c.unicode() < 0x3041 || c.unicode() > 0x309F) return false;
    return !s.isEmpty();
}

bool eligibleToken(const Token& t) { return t.kind == Token::Kind::Noun && !t.weak; }

// "Docker"+"Compose" -> "Docker Compose" (Latin words need their space); CJK parts are concatenated.
QString joinSurfaces(const QList<Token>& run)
{
    QString out;
    for (int i = 0; i < run.size(); ++i) {
        const Token& t = run[i];
        if (i > 0 && run[i - 1].pos >= 0 && t.pos >= 0) {     // overlapping n-grams: フォ + ォル + ルダ -> フォルダ
            const int shared = run[i - 1].pos + run[i - 1].surface.size() - t.pos;
            if (shared > 0) { out += t.surface.mid(shared); continue; }
        }
        if (i > 0 && isLatinWord(run[i - 1].surface) && isLatinWord(t.surface)) out += ' ';
        out += t.surface;
    }
    return out;
}

struct Cand {
    QString display, wakati;
    int tokens = 1;
    int count = 0;
};

} // namespace

bool KeywordExtractor::isStopword(const QString& lowerKey) { return stopwords().contains(lowerKey); }

QList<Keyword> KeywordExtractor::extract(const QList<Token>& tokens, const CorpusStats& stats, int topK)
{
    QHash<QString, Cand> cands;
    auto addRun = [&](const QList<Token>& run) {
        if (run.isEmpty()) return;
        const QString display = joinSurfaces(run);
        const QString key = ITokenizer::normalize(display).toLower();
        if (key.size() < 2 || isStopword(key) || allHiragana(key)) return;
        // a single token that is a stopword is covered above; also drop runs made only of short Latin words ("is a")
        QStringList wk;
        for (const Token& t : run) wk << t.surface;
        Cand& c = cands[key];
        if (c.count == 0) { c.display = display; c.wakati = wk.join(' '); c.tokens = int(run.size()); }
        c.count++;
    };

    QList<Token> run;
    auto flush = [&] {
        if (run.size() > kMaxRunTokens) {
            for (const Token& t : run) addRun({ t }); // a list of nouns, not one compound
        } else {
            // a compound may not start or end with a stopword ("the Docker" -> Docker)
            int b = 0, e = int(run.size());
            while (b < e && isStopword(run[b].surface.toLower())) ++b;
            while (e > b && isStopword(run[e - 1].surface.toLower())) --e;
            addRun(run.mid(b, e - b));
        }
        run.clear();
    };
    for (const Token& t : tokens) {
        if (eligibleToken(t)) run << t;
        else if (t.kind == Token::Kind::Suffix && !run.isEmpty()) run << t; // 担当 + 者 -> 担当者
        else flush();
    }
    flush();

    QList<Keyword> out;
    out.reserve(cands.size());
    const int n = qMax(0, stats.otherDocuments);
    for (auto it = cands.cbegin(); it != cands.cend(); ++it) {
        const Cand& c = it.value();
        const int df = (stats.docFreq && n > 0) ? qBound(0, stats.docFreq(c.wakati), n) : 0;
        const double idf = std::log((n + 1.0) / (df + 1.0)) + 1.0;       // 1 when everything has it, higher when rare
        const double tf = 1.0 + std::log(double(c.count));
        const double compound = 1.0 + 0.15 * (c.tokens - 1);             // prefer "共有フォルダ" over "共有"
        out.append({ c.display, it.key(), c.wakati, tf * idf * compound, c.count });
    }
    std::sort(out.begin(), out.end(), [](const Keyword& a, const Keyword& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.count != b.count) return a.count > b.count;
        return a.key < b.key;
    });
    if (topK >= 0 && out.size() > topK) out = out.mid(0, topK);
    return out;
}

} // namespace ake
