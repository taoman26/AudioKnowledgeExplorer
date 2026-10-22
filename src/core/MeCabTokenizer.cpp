#include "core/MeCabTokenizer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QStringList>
#include <mecab.h>

namespace ake {

struct MeCabTokenizer::Impl {
    mecab_t* mecab = nullptr;
    std::mutex mutex;
    ~Impl() { if (mecab) mecab_destroy(mecab); }
};

MeCabTokenizer::~MeCabTokenizer() = default;

std::unique_ptr<MeCabTokenizer> MeCabTokenizer::create(const QString& dicDir, QString* error)
{
    QString dir = dicDir.isEmpty() ? qEnvironmentVariable("AKE_MECAB_DICDIR") : dicDir;
    QByteArray args = "mecab";
    if (dir.isEmpty() && QCoreApplication::instance()) {
        // the dictionary packed next to the executable (AppImage: <app>/usr/share/AudioKnowledgeExplorer/mecab/jumandic); it
        // comes with its own empty rc file so that no /etc/mecabrc is needed either
        const QString packed = QDir(QCoreApplication::applicationDirPath()).filePath("../share/AudioKnowledgeExplorer/mecab/jumandic");
        if (QFile::exists(packed + "/sys.dic")) { dir = QDir::cleanPath(packed); args += " -r /dev/null"; }
    }
    if (!dir.isEmpty()) args += " -d " + QFile::encodeName(dir);
    std::unique_ptr<MeCabTokenizer> t(new MeCabTokenizer);
    t->m_impl = std::make_unique<Impl>();
    t->m_impl->mecab = mecab_new2(args.constData());
    if (!t->m_impl->mecab) {
        if (error) *error = QString::fromUtf8(mecab_strerror(nullptr));
        return nullptr;
    }
    // The dictionary must be UTF-8 (our strings are UTF-8); refuse others instead of producing mojibake.
    const mecab_dictionary_info_t* d = mecab_dictionary_info(t->m_impl->mecab);
    if (d && QByteArray(d->charset).toLower() != "utf-8" && QByteArray(d->charset).toLower() != "utf8") {
        if (error) *error = QStringLiteral("dictionary charset is %1, UTF-8 required").arg(QString::fromLatin1(d->charset));
        return nullptr;
    }
    return t;
}

namespace {
bool isHonorificOrPlural(const QString& s)
{
    static const QSet<QString> w = { "さん", "様", "さま", "君", "くん", "氏", "ちゃん", "殿", "達", "たち", "ら", "方" };
    return w.contains(s);
}

bool allAscii(const QString& s)
{
    for (QChar c : s) if (c.unicode() >= 0x80) return false;
    return !s.isEmpty();
}
} // namespace

QList<Token> MeCabTokenizer::tokenize(const QString& raw) const
{
    QList<Token> out;
    const QByteArray text = normalize(raw).toUtf8();
    if (text.trimmed().isEmpty()) return out;

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const mecab_node_t* node = mecab_sparse_tonode2(m_impl->mecab, text.constData(), size_t(text.size()));
    for (; node; node = node->next) {
        if (node->stat == MECAB_BOS_NODE || node->stat == MECAB_EOS_NODE) continue;
        Token t;
        t.surface = QString::fromUtf8(node->surface, int(node->length));
        const QStringList f = QString::fromUtf8(node->feature).split(',');
        const QString pos = f.value(0), sub = f.value(1);
        // JUMAN features: 品詞,細分類,活用型,活用形,原形,読み,代表表記...
        const QString base = f.value(4);
        t.base = (base.isEmpty() || base == "*") ? t.surface : base;
        const QString yomi = f.value(5);
        if (!yomi.isEmpty() && yomi != "*" && pos != "未定義語" && pos != "特殊") t.reading = toHiragana(yomi);
        if (pos == "名詞") {
            if (sub == "数詞") t.kind = Token::Kind::Number;
            else {
                t.kind = Token::Kind::Noun;
                t.weak = (sub == "形式名詞" || sub == "副詞的名詞" || sub == "時相名詞");
            }
        } else if (pos == "接尾辞" && sub == "名詞性名詞接尾辞" && !isHonorificOrPlural(t.surface)) {
            t.kind = Token::Kind::Suffix;
        } else if (pos == "特殊") t.kind = Token::Kind::Punct;
        else if (pos == "動詞") t.kind = Token::Kind::Verb;
        else if (pos == "形容詞") t.kind = Token::Kind::Adjective;
        else if (pos == "未定義語") {
            // unknown to the dictionary: Latin words are likely product/technical names, the rest is noise
            t.kind = allAscii(t.surface) ? Token::Kind::Noun : Token::Kind::Other;
        } else t.kind = Token::Kind::Other;
        if (t.surface.trimmed().isEmpty()) continue;
        out << t;
    }
    return out;
}

} // namespace ake
