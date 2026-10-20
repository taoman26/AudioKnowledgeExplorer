#include "core/RuleBasedTitleGenerator.h"

#include <QFileInfo>
#include <QRegularExpression>

namespace ake {

namespace {
struct Kind {
    const char* label;     // appended to the title
    const char* pathRe;    // matched against the lower-cased path
    const char* speechRe;  // counted in the transcript
};
const Kind kKinds[] = {
    { "会議", "meeting|mtg|会議|ミーティング|打ち合わせ|定例", "会議|ミーティング|打ち合わせ|定例" },
    { "勉強会", "study|seminar|workshop|勉強会|セミナー|講座|レクチャー", "勉強会|セミナー|講座|ハンズオン" },
    { "面接", "interview|面接|インタビュー", "面接|インタビュー|志望|ご経歴" },
    { "ポッドキャスト", "podcast|ポッドキャスト|radio|ラジオ", "ポッドキャスト|リスナー|今週のゲスト|お便り" },
    { "メモ", "memo|voice|ボイスメモ|メモ|note", "" },   // voice memos have no tell-tale speech pattern
};

bool isCjk(QChar c) { return c.unicode() >= 0x3040; }
} // namespace

QString RuleBasedTitleGenerator::detectKind(const QString& relPath, const QStringList& segments)
{
    const QString path = relPath.toLower();
    for (const Kind& k : kKinds)
        if (QRegularExpression(k.pathRe).match(path).hasMatch()) return QString::fromUtf8(k.label);
    QString text = segments.join(' ');
    int best = 1;                  // needs at least two mentions to count
    QString label;
    for (const Kind& k : kKinds) {
        if (!*k.speechRe) continue;
        int n = 0;
        auto it = QRegularExpression(k.speechRe).globalMatch(text);
        while (it.hasNext()) { it.next(); ++n; }
        if (n > best) { best = n; label = QString::fromUtf8(k.label); }
    }
    return label;
}

QString RuleBasedTitleGenerator::title(const TitleInput& in) const
{
    if (in.keywords.isEmpty()) return {};
    const Keyword& k1 = in.keywords.first();
    QString topic = k1.term;
    // a second keyword only when it is nearly as strong, different from the first, and fits
    for (int i = 1; i < in.keywords.size(); ++i) {
        const Keyword& k2 = in.keywords[i];
        if (k2.score < 0.6 * k1.score) break;
        const QString a = k1.key, b = k2.key;
        if (a.contains(b) || b.contains(a)) continue;
        if (QString(k1.term + "・" + k2.term).size() <= kMaxTitleChars - 4) topic = k1.term + "・" + k2.term;
        break;
    }
    QString kind = detectKind(in.relPath, in.segments);
    if (!kind.isEmpty() && topic.contains(kind)) kind.clear();    // keyword already says it
    if (topic.size() > kMaxTitleChars) topic = topic.left(kMaxTitleChars - 1) + QChar(0x2026);
    if (kind.isEmpty()) return topic;
    return isCjk(topic.back()) ? topic + kind : topic + ' ' + kind;  // "移行・Podman 会議" vs "移行会議"
}

} // namespace ake
