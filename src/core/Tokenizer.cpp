#include "core/ITokenizer.h"

#include <QStringList>

#include "core/NgramTokenizer.h"
#ifdef AKE_HAVE_MECAB
#include "core/MeCabTokenizer.h"
#endif

namespace ake {

QString ITokenizer::normalize(const QString& text)
{
    return text.normalized(QString::NormalizationForm_KC);
}

QString ITokenizer::wakati(const QString& text) const
{
    QStringList parts;
    for (const Token& t : tokenize(text))
        if (t.kind != Token::Kind::Punct && !t.surface.trimmed().isEmpty())
            parts << t.surface;
    return parts.join(' ');
}

QString ITokenizer::toHiragana(const QString& text)
{
    QString out = text;
    for (QChar& c : out) {
        const ushort u = c.unicode();
        if ((u >= 0x30A1 && u <= 0x30F6) || u == 0x30FD || u == 0x30FE) c = QChar(u - 0x60);   // ァ..ヶ ヽヾ -> ぁ..ゖ ゝゞ
    }
    return out;                                                                                // ー (0x30FC) stays: the long vowel mark
}

QString ITokenizer::phonetic(const QString& text) const
{
    QString out;
    int lastEnd = -1;                                // end of the previous token in the text (overlapping n-grams)
    for (const Token& t : tokenize(text)) {
        if (t.kind == Token::Kind::Punct || t.surface.trimmed().isEmpty()) continue;
        if (t.reading.isEmpty()) {
            QString s = toHiragana(t.surface).toLower();
            if (t.pos >= 0 && t.pos < lastEnd) s = s.mid(lastEnd - t.pos);   // フォ + ォル + ルダ: write the shared kana once
            out += s;
        } else {
            out += t.reading;
        }
        if (t.pos >= 0) lastEnd = t.pos + int(t.surface.size());
    }
    return out;
}

std::unique_ptr<ITokenizer> createTokenizer(QString* note)
{
    if (qEnvironmentVariable("AKE_TOKENIZER") == QLatin1String("ngram")) {   // test the fallback (Haiku has no MeCab) on a machine that has it
        if (note) *note = QStringLiteral("AKE_TOKENIZER=ngram: using the n-gram fallback");
        return std::make_unique<NgramTokenizer>();
    }
#ifdef AKE_HAVE_MECAB
    QString err;
    if (auto m = MeCabTokenizer::create(QString(), &err))
        return m;
    if (note) *note = QStringLiteral("MeCab unavailable (%1); using the n-gram fallback").arg(err);
#else
    if (note) *note = QStringLiteral("built without MeCab; using the n-gram fallback");
#endif
    return std::make_unique<NgramTokenizer>();
}

} // namespace ake
