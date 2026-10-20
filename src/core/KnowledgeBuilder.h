#pragma once
#include <QList>
#include <QString>
#include <memory>

#include "core/ISummarizer.h"
#include "core/TextIndexer.h"

namespace ake {

class Database;
class ITokenizer;

// Everything derived from a finished transcript, in one call: search index + keywords + summary + title, then the
// per-recording search row. Re-runnable; user-written title/summary are kept.
class KnowledgeBuilder {
public:
    struct Result {
        int segments = 0;
        QList<Keyword> keywords;
        QString summary, title;       // what was generated (may be empty)
        bool keptUserTitle = false, keptUserSummary = false;
    };

    // Defaults: ExtractiveSummarizer and RuleBasedTitleGenerator. Not owned: db, tokenizer.
    KnowledgeBuilder(Database& db, const ITokenizer& tokenizer);
    ~KnowledgeBuilder();
    void setSummarizer(std::unique_ptr<ISummarizer> s);
    void setTitleGenerator(std::unique_ptr<ITitleGenerator> t);

    bool build(qint64 recordingId, Result* out = nullptr, QString* error = nullptr);

private:
    Database& m_db;
    const ITokenizer& m_tok;
    std::unique_ptr<ISummarizer> m_summarizer;
    std::unique_ptr<ITitleGenerator> m_titles;
};

} // namespace ake
