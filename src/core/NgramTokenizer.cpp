#include "core/NgramTokenizer.h"

#include <algorithm>

namespace ake {

namespace {
enum class Cls { Latin, Digit, Han, Katakana, Hiragana, Punct, Space };

bool isHan(char32_t c) { return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || c == 0x3005; }

Cls classify(QChar ch)
{
    const ushort c = ch.unicode();
    if (ch.isSpace()) return Cls::Space;
    if (c < 0x80) return ch.isLetter() ? Cls::Latin : ch.isDigit() ? Cls::Digit : Cls::Punct;
    if (c >= 0x3040 && c <= 0x309F) return Cls::Hiragana;
    if ((c >= 0x30A0 && c <= 0x30FF) || (c >= 0xFF66 && c <= 0xFF9F)) return Cls::Katakana; // incl. ー
    if (isHan(c)) return Cls::Han;
    if (ch.isLetter()) return Cls::Latin; // accented letters, Greek, ...
    if (ch.isDigit()) return Cls::Digit;
    return Cls::Punct;
}
} // namespace

QList<Token> NgramTokenizer::tokenize(const QString& raw) const
{
    const QString text = normalize(raw);
    QList<Token> out;
    int i = 0;
    const int n = text.size();
    while (i < n) {
        const Cls c = classify(text[i]);
        if (c == Cls::Space) { ++i; continue; }
        int j = i + 1;
        if (c == Cls::Latin || c == Cls::Digit) {
            // a word: letters and digits together ("GPT4", "systemd"); an inner ' or - keeps the word together
            while (j < n) {
                const Cls d = classify(text[j]);
                if (d == Cls::Latin || d == Cls::Digit) { ++j; continue; }
                break;
            }
            Token t;
            t.surface = t.base = text.mid(i, j - i);
            const bool allDigits = std::all_of(t.surface.begin(), t.surface.end(), [](QChar q) { return q.isDigit(); });
            t.kind = allDigits ? Token::Kind::Number : Token::Kind::Noun;
            out << t;
        } else if (c == Cls::Han || c == Cls::Katakana) {
            // a run of one script ("共有" | "フォルダ": never mixed, or bigrams like "有フ" appear) -> overlapping bigrams
            while (j < n && classify(text[j]) == c) ++j;
            const QString run = text.mid(i, j - i);
            if (run.size() == 1) {
                out << Token{ run, run, Token::Kind::Noun, true }; // a lone character: searchable, not a keyword
            } else {
                for (int k = 0; k + 1 < run.size(); ++k) {
                    const QString bg = run.mid(k, 2);
                    out << Token{ bg, bg, Token::Kind::Noun, false, i + k };
                }
            }
        } else if (c == Cls::Hiragana) {
            while (j < n && classify(text[j]) == Cls::Hiragana) ++j;
            const QString run = text.mid(i, j - i);
            out << Token{ run, run, Token::Kind::Other, false };
        } else {
            out << Token{ QString(text[i]), QString(text[i]), Token::Kind::Punct, false };
        }
        i = j;
    }
    return out;
}

} // namespace ake
