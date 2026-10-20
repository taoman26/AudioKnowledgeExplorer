#pragma once
#include <QSqlDatabase>
#include <QString>

namespace ake {

// Owns one SQLite connection (QtSql) and brings the schema up to date.
// Schema follows design.md section 15. FTS tables hold text already tokenized by the app
// (MeCab word-split, space separated) and are tokenized by FTS5's `unicode61`.
class Database {
public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Opens (creating if needed) the file; ":memory:" is allowed. Applies pragmas and migrations.
    bool open(const QString& path, QString* error = nullptr);
    void close();
    bool isOpen() const { return m_db.isOpen(); }

    QSqlDatabase& handle() { return m_db; }
    int schemaVersion() const;
    static int latestSchemaVersion();

    QString lastError() const { return m_error; }

private:
    bool migrate();
    bool fail(const QString& msg);

    QSqlDatabase m_db;
    QString m_connName;
    QString m_error;
};

} // namespace ake
