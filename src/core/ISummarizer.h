#pragma once
#include <QList>
#include <QString>
#include <QStringList>

#include "core/KeywordExtractor.h"

namespace ake {

class ITokenizer;

struct SummaryOptions {
    int maxSentences = 3;
    int maxChars = 220;   // the card shows ~3 lines and elides; the detail pane shows all of it
};

struct SummaryInput {
    QStringList segments;          // transcript text in order (user corrections already applied)
    const ITokenizer* tokenizer = nullptr;
    QList<Keyword> keywords;       // best first; used to favour sentences about the main topics
};

// Replaceable on purpose: the extractive implementation is the only one planned, but nothing here assumes it.
class ISummarizer {
public:
    virtual ~ISummarizer() = default;
    virtual QString name() const = 0;
    virtual QString summarize(const SummaryInput& in, const SummaryOptions& opt = {}) const = 0; // "" when nothing to say
};

struct TitleInput {
    QList<Keyword> keywords;       // best first
    QString relPath;               // "meetings/2026-09/standup.m4a": folder and file names hint at the kind of recording
    QStringList segments;
};

class ITitleGenerator {
public:
    virtual ~ITitleGenerator() = default;
    virtual QString name() const = 0;
    virtual QString title(const TitleInput& in) const = 0;  // "" when there is nothing better than the file name
};

} // namespace ake
