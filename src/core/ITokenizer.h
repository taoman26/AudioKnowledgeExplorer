#pragma once
#include <QList>
#include <QString>
#include <memory>

namespace ake {

struct Token {
    // Suffix: a noun suffix that belongs to the preceding noun (担当 + 者, 社 + 内); honorifics are Other.
    enum class Kind { Noun, Number, Verb, Adjective, Other, Punct, Suffix };
    QString surface;
    QString base;       // dictionary form when known, else surface
    Kind kind = Kind::Other;
    bool weak = false;  // a noun that is useless as a keyword (形式名詞, 時相名詞, 副詞的名詞, ...)
    // Where the token sits in the normalized text, only for tokenizers whose tokens overlap (n-grams): neighbours that
    // overlap are joined without repeating the shared characters. -1 = unknown (a dictionary tokenizer: never overlaps).
    int pos = -1;
    QString reading;    // how it is read, in hiragana, when the tokenizer knows (a dictionary tokenizer); empty otherwise
};

// Splits text into words. All implementations first normalize with NFKC (full-width -> ASCII, half-width kana ->
// full-width), so indexing and querying agree. Implementations must be thread-safe.
class ITokenizer {
public:
    virtual ~ITokenizer() = default;
    virtual QString name() const = 0;
    virtual QList<Token> tokenize(const QString& text) const = 0;
    virtual bool hasReadings() const { return false; }   // tokens carry `reading` (needed by phonetic search)

    // Space separated surfaces without punctuation: what goes into FTS5 (unicode61 splits on the spaces).
    QString wakati(const QString& text) const;
    static QString normalize(const QString& text); // NFKC

    // The text as it sounds: each word's reading in hiragana (surface when unknown: Latin, digits, unknown words),
    // lower-cased, punctuation and spaces dropped. "接続検討" and "設定を検討" share "けんとう"; a homophone typed
    // with other kanji ("検討" / "見当") has the same phonetic form. Without a dictionary only the kana are unified.
    QString phonetic(const QString& text) const;
    static QString toHiragana(const QString& text);
};

// MeCab when it can be loaded (dictionary found), otherwise the character n-gram fallback.
std::unique_ptr<ITokenizer> createTokenizer(QString* note = nullptr);

} // namespace ake
