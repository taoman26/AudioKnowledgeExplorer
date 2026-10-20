#pragma once
#include "core/ISummarizer.h"

namespace ake {

// "Docker・Podman会議": the two strongest keywords + the kind of recording when it can be told (from the folder/file
// name first, then from how often the transcript says 会議 / 勉強会 / ...). Empty when there are no keywords.
class RuleBasedTitleGenerator : public ITitleGenerator {
public:
    QString name() const override { return QStringLiteral("rule-based-v1"); }
    QString title(const TitleInput& in) const override;

    static constexpr int kMaxTitleChars = 28;
    // The detected kind ("会議", "勉強会", ...) or empty; exposed for tests.
    static QString detectKind(const QString& relPath, const QStringList& segments);
};

} // namespace ake
