#include "core/Database.h"

#include <QAtomicInt>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ake {

namespace {

struct Migration {
    int version;
    const char* sql; // ';'-separated statements
};

// Append-only. Never edit a released migration; add a new one.
const Migration kMigrations[] = {
    { 1, R"SQL(
CREATE TABLE folders(
    id INTEGER PRIMARY KEY,
    path TEXT NOT NULL UNIQUE,
    enabled INTEGER NOT NULL DEFAULT 1,
    recursive INTEGER NOT NULL DEFAULT 1,
    exclude_glob TEXT NOT NULL DEFAULT '',
    last_scan_at INTEGER
);
CREATE TABLE recordings(
    id INTEGER PRIMARY KEY,
    folder_id INTEGER NOT NULL REFERENCES folders(id) ON DELETE CASCADE,
    rel_path TEXT NOT NULL,
    abs_path TEXT NOT NULL UNIQUE,
    file_size INTEGER NOT NULL DEFAULT 0,
    mtime INTEGER NOT NULL DEFAULT 0,
    content_hash TEXT NOT NULL DEFAULT '',
    duration_ms INTEGER NOT NULL DEFAULT 0,
    format TEXT NOT NULL DEFAULT '',
    sample_rate INTEGER NOT NULL DEFAULT 0,
    recorded_at INTEGER,
    status TEXT NOT NULL DEFAULT 'new'
        CHECK(status IN ('new','queued','transcribing','analyzing','done','error')),
    error_msg TEXT NOT NULL DEFAULT '',
    favorite INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
CREATE INDEX idx_recordings_folder_date ON recordings(folder_id, recorded_at);
CREATE INDEX idx_recordings_status ON recordings(status);
CREATE INDEX idx_recordings_hash ON recordings(content_hash);
CREATE TABLE transcripts(
    id INTEGER PRIMARY KEY,
    recording_id INTEGER NOT NULL REFERENCES recordings(id) ON DELETE CASCADE,
    language TEXT NOT NULL DEFAULT '',
    model TEXT NOT NULL DEFAULT '',
    model_version TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    is_current INTEGER NOT NULL DEFAULT 1
);
CREATE INDEX idx_transcripts_recording ON transcripts(recording_id, is_current);
CREATE TABLE segments(
    id INTEGER PRIMARY KEY,
    transcript_id INTEGER NOT NULL REFERENCES transcripts(id) ON DELETE CASCADE,
    idx INTEGER NOT NULL,
    start_ms INTEGER NOT NULL,
    end_ms INTEGER NOT NULL,
    text TEXT NOT NULL,
    wakati TEXT NOT NULL DEFAULT '',
    edited_text TEXT,
    speaker TEXT
);
CREATE INDEX idx_segments_transcript ON segments(transcript_id, idx);
CREATE TABLE analyses(
    id INTEGER PRIMARY KEY,
    recording_id INTEGER NOT NULL REFERENCES recordings(id) ON DELETE CASCADE,
    kind TEXT NOT NULL CHECK(kind IN ('title','summary','chapters')),
    content TEXT NOT NULL,
    source TEXT NOT NULL CHECK(source IN ('ai-extractive','user')),
    model TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    is_current INTEGER NOT NULL DEFAULT 1
);
CREATE INDEX idx_analyses_recording ON analyses(recording_id, kind, is_current);
CREATE TABLE keywords(
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,
    normalized TEXT NOT NULL DEFAULT ''
);
CREATE TABLE recording_keywords(
    recording_id INTEGER NOT NULL REFERENCES recordings(id) ON DELETE CASCADE,
    keyword_id INTEGER NOT NULL REFERENCES keywords(id) ON DELETE CASCADE,
    score REAL NOT NULL DEFAULT 0,
    source TEXT NOT NULL DEFAULT 'ai' CHECK(source IN ('ai','user')),
    PRIMARY KEY(recording_id, keyword_id)
);
CREATE INDEX idx_recording_keywords_kw ON recording_keywords(keyword_id);
CREATE TABLE chapters(
    id INTEGER PRIMARY KEY,
    recording_id INTEGER NOT NULL REFERENCES recordings(id) ON DELETE CASCADE,
    start_ms INTEGER NOT NULL,
    title TEXT NOT NULL,
    source TEXT NOT NULL DEFAULT 'ai' CHECK(source IN ('ai','user'))
);
CREATE INDEX idx_chapters_recording ON chapters(recording_id, start_ms);
CREATE TABLE notes(
    recording_id INTEGER PRIMARY KEY REFERENCES recordings(id) ON DELETE CASCADE,
    body_md TEXT NOT NULL DEFAULT '',
    updated_at INTEGER NOT NULL
);
CREATE TABLE jobs(
    id INTEGER PRIMARY KEY,
    recording_id INTEGER NOT NULL REFERENCES recordings(id) ON DELETE CASCADE,
    stage TEXT NOT NULL,
    state TEXT NOT NULL DEFAULT 'queued',
    progress REAL NOT NULL DEFAULT 0,
    attempts INTEGER NOT NULL DEFAULT 0,
    last_error TEXT NOT NULL DEFAULT '',
    updated_at INTEGER NOT NULL
);
CREATE INDEX idx_jobs_state ON jobs(state);
CREATE TABLE settings(
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE VIRTUAL TABLE fts_segments USING fts5(wakati, tokenize='unicode61');
CREATE VIRTUAL TABLE fts_recording USING fts5(title, summary, tags, notes, tokenize='unicode61')
)SQL" },
    // v2: keep the (contentless-link) FTS tables in step when rows disappear, incl. cascades from folder/recording deletes
    { 2, R"SQL(
CREATE TRIGGER segments_ad AFTER DELETE ON segments BEGIN
    DELETE FROM fts_segments WHERE rowid = old.id;
END;
CREATE TRIGGER recordings_ad AFTER DELETE ON recordings BEGIN
    DELETE FROM fts_recording WHERE rowid = old.id;
END;
)SQL" },
    // v3: keywords the user removed from a recording must not come back when it is analysed again
    { 3, R"SQL(
CREATE TABLE keyword_blocks(
    recording_id INTEGER NOT NULL REFERENCES recordings(id) ON DELETE CASCADE,
    keyword_id INTEGER NOT NULL REFERENCES keywords(id) ON DELETE CASCADE,
    PRIMARY KEY(recording_id, keyword_id)
)
)SQL" },
    // v4: how each segment sounds (hiragana readings), to find words the recogniser wrote as a similar-sounding other word
    { 4, R"SQL(
ALTER TABLE segments ADD COLUMN phonetic TEXT NOT NULL DEFAULT ''
)SQL" },
};

// Statements end at a line ending in ';'. A CREATE TRIGGER body contains ';' itself, so it ends at the line "END;".
QStringList splitStatements(const QString& script)
{
    QStringList out;
    QString buf;
    for (const QString& line : script.split('\n')) {
        buf += line + '\n';
        const QString t = line.trimmed();
        const bool trigger = buf.trimmed().startsWith("CREATE TRIGGER", Qt::CaseInsensitive);
        if (trigger ? t.compare("END;", Qt::CaseInsensitive) == 0 : t.endsWith(';')) {
            out << buf.trimmed();
            buf.clear();
        }
    }
    if (!buf.trimmed().isEmpty()) out << buf.trimmed(); // last statement without a terminator
    return out;
}

QAtomicInt g_connCounter;

} // namespace

Database::~Database() { close(); }

int Database::latestSchemaVersion()
{
    return kMigrations[sizeof(kMigrations) / sizeof(kMigrations[0]) - 1].version;
}

bool Database::fail(const QString& msg)
{
    m_error = msg;
    return false;
}

bool Database::open(const QString& path, QString* error)
{
    close();
    m_error.clear();
    m_connName = QStringLiteral("ake_db_%1").arg(g_connCounter.fetchAndAddRelaxed(1));
    m_db = QSqlDatabase::addDatabase("QSQLITE", m_connName);
    m_db.setDatabaseName(path);
    bool ok = m_db.open();
    if (!ok)
        fail(m_db.lastError().text());
    if (ok) {
        QSqlQuery q(m_db);
        // WAL is a no-op for :memory:. foreign_keys is per-connection and must be set every time.
        for (const char* pragma : { "PRAGMA foreign_keys=ON", "PRAGMA journal_mode=WAL", "PRAGMA synchronous=NORMAL" }) {
            if (!q.exec(pragma)) { ok = fail(q.lastError().text()); break; }
        }
    }
    ok = ok && migrate();
    if (!ok) {
        if (error) *error = m_error;
        close();
    }
    return ok;
}

void Database::close()
{
    if (m_connName.isEmpty())
        return;
    if (m_db.isOpen())
        m_db.close();
    m_db = QSqlDatabase(); // release before removeDatabase
    QSqlDatabase::removeDatabase(m_connName);
    m_connName.clear();
}

int Database::schemaVersion() const
{
    QSqlQuery q(m_db);
    if (!q.exec("SELECT COALESCE(MAX(version),0) FROM schema_migrations") || !q.next())
        return 0;
    return q.value(0).toInt();
}

bool Database::migrate()
{
    QSqlQuery q(m_db);
    if (!q.exec("CREATE TABLE IF NOT EXISTS schema_migrations(version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL)"))
        return fail(q.lastError().text());

    const int current = schemaVersion();
    if (current > latestSchemaVersion())
        return fail(QStringLiteral("database schema v%1 is newer than this app (v%2)")
                        .arg(current).arg(latestSchemaVersion()));

    for (const Migration& m : kMigrations) {
        if (m.version <= current)
            continue;
        if (!m_db.transaction())
            return fail(m_db.lastError().text());
        bool ok = true;
        for (const QString& stmt : splitStatements(QString::fromUtf8(m.sql))) {
            if (!q.exec(stmt)) {
                ok = fail(QStringLiteral("migration v%1 failed: %2 [%3]")
                              .arg(m.version).arg(q.lastError().text(), stmt.trimmed().left(60)));
                break;
            }
        }
        if (ok) {
            q.prepare("INSERT INTO schema_migrations(version, applied_at) VALUES(?, strftime('%s','now'))");
            q.addBindValue(m.version);
            if (!q.exec())
                ok = fail(q.lastError().text());
        }
        if (!ok) {
            m_db.rollback();
            return false;
        }
        if (!m_db.commit())
            return fail(m_db.lastError().text());
    }
    return true;
}

} // namespace ake
