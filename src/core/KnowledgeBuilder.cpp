#include "core/KnowledgeBuilder.h"

#include <QSqlQuery>

#include "core/AnalysisRepository.h"
#include "core/Database.h"
#include "core/ExtractiveSummarizer.h"
#include "core/ITokenizer.h"
#include "core/RuleBasedTitleGenerator.h"
#include "core/TranscriptCleaner.h"
#include "core/TranscriptRepository.h"

namespace ake {

KnowledgeBuilder::KnowledgeBuilder(Database& db, const ITokenizer& tokenizer)
    : m_db(db), m_tok(tokenizer), m_summarizer(new ExtractiveSummarizer), m_titles(new RuleBasedTitleGenerator)
{
}

KnowledgeBuilder::~KnowledgeBuilder() = default;
void KnowledgeBuilder::setSummarizer(std::unique_ptr<ISummarizer> s) { m_summarizer = std::move(s); }
void KnowledgeBuilder::setTitleGenerator(std::unique_ptr<ITitleGenerator> t) { m_titles = std::move(t); }

bool KnowledgeBuilder::build(qint64 id, Result* out, QString* error)
{
    TextIndexer indexer(m_db, m_tok);
    TextIndexer::Result idx;
    if (!indexer.index(id, &idx, error)) return false;

    TranscriptResult transcript;
    if (!TranscriptRepository(m_db).loadCurrent(id, &transcript)) {
        if (error) *error = "transcript vanished while building knowledge";
        return false;
    }
    QStringList texts;
    for (const TranscriptSegment& s : transcript.segments) texts << s.text;
    texts = TranscriptCleaner::clean(texts);   // no repeated hallucinations in the summary

    QSqlQuery q(m_db.handle());
    q.prepare("SELECT rel_path FROM recordings WHERE id=?");
    q.addBindValue(id);
    const QString relPath = (q.exec() && q.next()) ? q.value(0).toString() : QString();

    Result r;
    r.segments = idx.segments;
    r.keywords = idx.keywords;
    r.summary = m_summarizer->summarize({ texts, &m_tok, idx.keywords });
    r.title = m_titles->title({ idx.keywords, relPath, texts });

    AnalysisRepository repo(m_db);
    QString err;
    // An empty result is not stored: the card then shows the file name / no summary rather than a blank AI entry.
    if (r.summary.isEmpty()) repo.clearAi(id, AnalysisRepository::Summary);     // ... and an earlier AI text must go
    if (r.title.isEmpty()) repo.clearAi(id, AnalysisRepository::Title);
    if (!r.summary.isEmpty() && !repo.saveAi(id, AnalysisRepository::Summary, r.summary, m_summarizer->name(), &r.keptUserSummary, &err)) {
        if (error) *error = err;
        return false;
    }
    if (!r.title.isEmpty() && !repo.saveAi(id, AnalysisRepository::Title, r.title, m_titles->name(), &r.keptUserTitle, &err)) {
        if (error) *error = err;
        return false;
    }
    if (!indexer.rebuildRecordingFts(id, &err)) { if (error) *error = err; return false; }   // title/summary are searchable
    if (out) *out = r;
    return true;
}

} // namespace ake
