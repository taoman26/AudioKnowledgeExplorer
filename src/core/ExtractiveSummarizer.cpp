#include "core/ExtractiveSummarizer.h"

#include <QHash>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include "core/ITokenizer.h"

namespace ake {

namespace {

bool isAsciiWordChar(QChar c) { return c.unicode() < 0x80 && c.isLetterOrNumber(); }
bool isTerminator(QChar c) { return c == QChar(0x3002) || c == QChar(0xFF01) || c == QChar(0xFF1F) || c == '!' || c == '?'; }
bool isCloser(QChar c)
{
    static const QString closers = QString::fromUtf8("」』）)”\"’'】");
    return closers.contains(c);
}

// "えー、まず" -> "まず": leading fillers add nothing to a summary
QString stripFillers(QString s)
{
    static const QRegularExpression filler(QString::fromUtf8("^(?:えー+|ええと|えっと|あのー*|その|まあ|そうですね|はい)[、,\\s]*"));
    for (int i = 0; i < 3; ++i) {
        const QString t = s.remove(filler);
        if (t == s) break;
        s = t;
    }
    return s.trimmed();
}

struct Sent {
    QString text;
    int index = 0;                       // position in the original order
    std::vector<std::pair<int, float>> vec; // term id -> tf-idf weight (L2 normalized)
    double rank = 0;
};

} // namespace

QStringList ExtractiveSummarizer::splitSentences(const QStringList& segments, int forceBreakChars)
{
    QStringList out;
    QString cur;
    QList<int> joinAt;                              // where in `cur` another segment starts (not for the Latin word gaps)
    auto flush = [&] {
        const QString t = cur.trimmed();
        if (!t.isEmpty()) out << t;
        cur.clear();
        joinAt.clear();
    };
    // A sentence that is cut because it got long: its segment boundaries were pauses, so write them as commas
    auto flushForced = [&] {
        QString t;
        int from = 0;
        for (int pos : joinAt) {
            t += cur.mid(from, pos - from);
            const QChar prev = t.isEmpty() ? QChar() : t.back();
            // a fragment of a word or two ("で", "です", "私") belongs to what follows: no comma after it
            const bool longEnough = pos - from >= kMinPieceForComma;
            if (longEnough && !t.isEmpty() && prev != QChar(0x3001) && prev != QChar(0xFF0C) && prev != ',' && !isTerminator(prev)) t += QChar(0x3001);
            from = pos;
        }
        t += cur.mid(from);
        t = t.trimmed();
        if (!t.isEmpty() && !isTerminator(t.back()) && t.back() != QChar(0x3001) && t.back() != '.' && t.back() != QChar(0x2026))
            t += QChar(0x3002);
        else if (!t.isEmpty() && t.back() == QChar(0x3001)) t.back() = QChar(0x3002);
        if (!t.isEmpty()) out << t;
        cur.clear();
        joinAt.clear();
    };
    for (const QString& rawSeg : segments) {
        const QString seg = rawSeg.trimmed();
        if (seg.isEmpty()) continue;
        if (!cur.isEmpty() && isAsciiWordChar(cur.back()) && isAsciiWordChar(seg.front())) cur += ' '; // Latin words need a gap
        else if (!cur.isEmpty() && (cur.back() == ',' || cur.back() == '.' || cur.back() == '?' || cur.back() == '!') &&
                 seg.front().unicode() < 0x80) cur += ' ';
        else if (!cur.isEmpty()) joinAt << int(cur.size());          // a pause between two pieces (a comma, if it comes to that)
        for (int i = 0; i < seg.size(); ++i) {
            const QChar c = seg[i];
            cur += c;
            bool end = isTerminator(c);
            if (c == '.' || c == QChar(0xFF0E)) {   // ASCII period ends a sentence only before a gap and not in "3.5"
                const bool atEnd = i + 1 >= seg.size();
                const bool before = i > 0 && seg[i - 1].isDigit();
                const bool after = !atEnd && seg[i + 1].isDigit();
                end = (atEnd || seg[i + 1].isSpace()) && !(before && after);
            }
            if (end) {
                while (i + 1 < seg.size() && (isCloser(seg[i + 1]) || isTerminator(seg[i + 1]) || seg[i + 1] == '.')) cur += seg[++i];
                flush();
            }
        }
        if (cur.size() >= forceBreakChars) flushForced();   // unpunctuated speech: cut at the segment boundary
    }
    if (!joinAt.isEmpty() && !isTerminator(cur.trimmed().back())) flushForced();   // the unpunctuated tail of the transcript (2+ pieces)
    else flush();
    return out;
}

QString ExtractiveSummarizer::summarize(const SummaryInput& in, const SummaryOptions& opt) const
{
    const QStringList all = splitSentences(in.segments);
    if (all.isEmpty()) return {};

    // --- candidates: sentence-sized, with enough content words
    std::vector<Sent> cands;
    QHash<QString, int> termIds;
    std::vector<std::vector<int>> sentTerms;      // term ids per candidate (with repeats)
    for (int i = 0; i < all.size(); ++i) {
        const QString text = stripFillers(all[i]);
        if (text.size() < kMinChars || text.size() > kMaxChars) continue;
        QList<Token> toks = in.tokenizer ? in.tokenizer->tokenize(text) : QList<Token>{};
        std::vector<int> ids;
        for (const Token& t : toks) {
            if (t.kind != Token::Kind::Noun || t.weak) continue;
            const QString key = ITokenizer::normalize(t.surface).toLower();
            if (key.size() < 2 || KeywordExtractor::isStopword(key)) continue;
            ids.push_back(termIds.contains(key) ? termIds[key] : (termIds[key] = termIds.size()));
        }
        if (ids.size() < 3) continue;             // too little content: "ありがとうございました" and the like
        Sent s;
        s.text = text;
        s.index = i;
        cands.push_back(std::move(s));
        sentTerms.push_back(std::move(ids));
    }
    if (cands.empty()) {                          // nothing sentence-like: fall back to the beginning of the text
        QString head = all.join(all.first().front().unicode() < 0x80 ? " " : "");
        if (head.size() > opt.maxChars) head = head.left(opt.maxChars - 1) + QChar(0x2026);
        return head;
    }

    // --- cap the O(n^2) part on very long transcripts: keep the sentences richest in content words
    if (int(cands.size()) > kMaxCandidates) {
        std::vector<int> order(cands.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return sentTerms[a].size() > sentTerms[b].size(); });
        order.resize(kMaxCandidates);
        std::sort(order.begin(), order.end());
        std::vector<Sent> c2; std::vector<std::vector<int>> t2;
        for (int k : order) { c2.push_back(std::move(cands[k])); t2.push_back(std::move(sentTerms[k])); }
        cands = std::move(c2);
        sentTerms = std::move(t2);
    }
    const int n = int(cands.size());

    // --- tf-idf vectors (idf over sentences), L2 normalized
    std::vector<int> df(termIds.size(), 0);
    for (const auto& ids : sentTerms) {
        std::vector<int> u = ids;
        std::sort(u.begin(), u.end());
        u.erase(std::unique(u.begin(), u.end()), u.end());
        for (int id : u) df[id]++;
    }
    for (int i = 0; i < n; ++i) {
        QHash<int, int> tf;
        for (int id : sentTerms[i]) tf[id]++;
        double norm = 0;
        std::vector<std::pair<int, float>> v;
        for (auto it = tf.cbegin(); it != tf.cend(); ++it) {
            const double w = (1.0 + std::log(double(it.value()))) * std::log(1.0 + double(n) / df[it.key()]);
            v.push_back({ it.key(), float(w) });
            norm += w * w;
        }
        norm = std::sqrt(norm);
        for (auto& p : v) p.second = norm > 0 ? float(p.second / norm) : 0.f;
        std::sort(v.begin(), v.end());
        cands[i].vec = std::move(v);
    }
    auto cosine = [&](int a, int b) {
        const auto& x = cands[a].vec; const auto& y = cands[b].vec;
        size_t i = 0, j = 0; double dot = 0;
        while (i < x.size() && j < y.size()) {
            if (x[i].first == y[j].first) { dot += double(x[i].second) * y[j].second; ++i; ++j; }
            else if (x[i].first < y[j].first) ++i; else ++j;
        }
        return dot;
    };

    // --- similarity graph (via postings: only sentences sharing a term are compared)
    std::vector<std::vector<std::pair<int, float>>> adj(n);
    {
        std::vector<std::vector<int>> postings(termIds.size());
        for (int i = 0; i < n; ++i) for (const auto& p : cands[i].vec) postings[p.first].push_back(i);
        std::vector<char> seen(n, 0);
        for (int i = 0; i < n; ++i) {
            std::vector<int> touched;
            for (const auto& p : cands[i].vec)
                for (int j : postings[p.first])
                    if (j != i && !seen[j]) { seen[j] = 1; touched.push_back(j); }
            for (int j : touched) {
                seen[j] = 0;
                const double s = cosine(i, j);
                if (s > 0.02) adj[i].push_back({ j, float(s) });
            }
        }
    }

    // --- PageRank
    std::vector<double> outSum(n, 0), score(n, 1.0 / n), next(n);
    for (int i = 0; i < n; ++i) for (const auto& e : adj[i]) outSum[i] += e.second;
    const double d = 0.85;
    for (int iter = 0; iter < 50; ++iter) {
        std::fill(next.begin(), next.end(), (1.0 - d) / n);
        for (int j = 0; j < n; ++j) {
            if (outSum[j] <= 0) continue;
            for (const auto& e : adj[j]) next[e.first] += d * score[j] * e.second / outSum[j];
        }
        double delta = 0;
        for (int i = 0; i < n; ++i) delta += std::abs(next[i] - score[i]);
        score.swap(next);
        if (delta < 1e-8) break;
    }

    // --- adjust: topic keywords, length, position
    QStringList topKeys;
    for (int i = 0; i < in.keywords.size() && i < 5; ++i) topKeys << in.keywords[i].key;
    const double maxRank = *std::max_element(score.begin(), score.end());
    for (int i = 0; i < n; ++i) {
        const QString low = ITokenizer::normalize(cands[i].text).toLower();
        int hit = 0;
        for (const QString& k : topKeys) hit += low.contains(k);
        const double coverage = topKeys.isEmpty() ? 0 : double(hit) / topKeys.size();
        const int len = cands[i].text.size();
        const double lenFactor = len < 20 ? 0.6 : len <= 100 ? 1.0 : 1.0 / (1.0 + (len - 100) / 150.0);
        const double pos = double(cands[i].index) / std::max<qsizetype>(1, all.size());
        const double posFactor = pos < 0.1 ? 1.15 : pos > 0.95 ? 1.05 : 1.0;
        cands[i].rank = (maxRank > 0 ? score[i] / maxRank : 1.0) * lenFactor * (1.0 + 0.5 * coverage) * posFactor;
    }

    // --- greedy selection without near-duplicates, within the character budget
    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return cands[a].rank > cands[b].rank; });
    std::vector<int> chosen;
    int chars = 0;
    for (int idx : order) {
        if (int(chosen.size()) >= opt.maxSentences) break;
        bool dup = false;
        for (int c : chosen) if (cosine(idx, c) > 0.5) { dup = true; break; }
        if (dup) continue;
        const int len = cands[idx].text.size();
        if (!chosen.empty() && chars + len > opt.maxChars) continue;   // try a shorter one
        chosen.push_back(idx);
        chars += len;
    }
    std::sort(chosen.begin(), chosen.end(), [&](int a, int b) { return cands[a].index < cands[b].index; });
    QStringList parts;
    for (int c : chosen) parts << cands[c].text;
    QString summary = parts.join(parts.first().back().unicode() < 0x80 ? " " : "");
    if (summary.size() > opt.maxChars) summary = summary.left(opt.maxChars - 1) + QChar(0x2026);
    return summary;
}

} // namespace ake
