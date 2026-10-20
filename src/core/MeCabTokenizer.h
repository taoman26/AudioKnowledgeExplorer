#pragma once
#include <memory>
#include <mutex>

#include "core/ITokenizer.h"

namespace ake {

// MeCab with the JUMAN (or any UTF-8) dictionary. A single tagger guarded by a mutex: parsing is fast and the
// callers are not parse-bound.
class MeCabTokenizer : public ITokenizer {
public:
    ~MeCabTokenizer() override;
    // `dicDir` empty = MeCab's default (mecabrc). Env AKE_MECAB_DICDIR overrides when `dicDir` is empty.
    static std::unique_ptr<MeCabTokenizer> create(const QString& dicDir = QString(), QString* error = nullptr);

    QString name() const override { return QStringLiteral("mecab"); }
    QList<Token> tokenize(const QString& text) const override;
    bool hasReadings() const override { return true; }

private:
    MeCabTokenizer() = default;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ake
