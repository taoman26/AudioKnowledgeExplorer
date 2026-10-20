#include "core/TranscriptCleaner.h"

#include <QRegularExpression>

namespace ake {

namespace {
// The comparison ignores punctuation and spacing: "はい、" and "はい。" are the same utterance.
QString fingerprint(const QString& s)
{
    QString out;
    for (QChar c : s)
        if (c.isLetterOrNumber()) out += c.toLower();
    return out;
}
} // namespace

QString TranscriptCleaner::stripNonSpeechTags(const QString& text)
{
    static const QRegularExpression tag(QString::fromUtf8("[\\[［【][^\\]］】\\n]{1,20}[\\]］】]"));
    if (!text.contains(tag)) return text;           // untouched when there is nothing to remove
    QString t = text;
    t.remove(tag);
    return t.simplified();
}

QString TranscriptCleaner::collapseLoops(const QString& text)
{
    // a phrase of 3..60 chars (any chars) followed by itself at least twice more
    static const QRegularExpression loop(QStringLiteral("(.{3,60}?)\\1{2,}"), QRegularExpression::DotMatchesEverythingOption);
    QString t = text;
    for (int i = 0; i < 5; ++i) {                  // nested loops need more than one pass
        const QString next = QString(t).replace(loop, QStringLiteral("\\1"));
        if (next == t) break;
        t = next;
    }
    return t;
}

// Whisper can also get stuck on a cycle of several phrases ("私は、" "私の力を抜いて、" "私は、" ... hundreds of times).
// A cycle of 2..kMaxCycle segments that repeats at least kMinCycles times in a row is a loop: only its first round stays.
// (Two rounds, "A B A B", is ordinary conversation and is kept.)
QList<bool> TranscriptCleaner::loopMask(const QStringList& fingerprints)
{
    const int n = int(fingerprints.size());
    QList<bool> drop(n, false);
    for (int p = 2; p <= kMaxCycle; ++p) {
        int run = 0;                               // consecutive i with fp[i] == fp[i - p]
        for (int i = p; i <= n; ++i) {
            const bool same = i < n && !fingerprints[i].isEmpty() && fingerprints[i] == fingerprints[i - p];
            if (same) { ++run; continue; }
            if (run >= p * (kMinCycles - 1))       // the first round plus (kMinCycles - 1) more rounds
                for (int k = i - run; k < i; ++k) drop[k] = true;
            run = 0;
        }
    }
    return drop;
}

QStringList TranscriptCleaner::clean(const QStringList& segments)
{
    QStringList texts, fps;
    for (const QString& raw : segments) {
        texts << collapseLoops(stripNonSpeechTags(raw).trimmed());
        fps << fingerprint(texts.last());
    }
    const QList<bool> cycle = loopMask(fps);

    QStringList out;
    QStringList recent;                            // fingerprints of the last kept segments
    for (int i = 0; i < texts.size(); ++i) {
        const QString& text = texts[i];
        const QString& fp = fps[i];
        if (fp.isEmpty() || cycle[i]) continue;
        const bool sameAsPrevious = !recent.isEmpty() && recent.last() == fp;   // any length: "はい", "はい" is one utterance
        const bool echoOfRecent = fp.size() >= 8 && recent.contains(fp);
        if (sameAsPrevious || echoOfRecent) continue;
        out << text;
        recent << fp;
        if (recent.size() > 3) recent.removeFirst();
    }
    return out;
}

} // namespace ake
