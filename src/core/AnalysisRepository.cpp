#include "core/AnalysisRepository.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "core/Database.h"
#include "core/SqlText.h"

namespace ake {

bool AnalysisRepository::current(qint64 id, Kind kind, Entry* out) const
{
    QSqlQuery q(m_db.handle());
    q.prepare("SELECT content, source FROM analyses WHERE recording_id=? AND kind=? AND is_current=1 ORDER BY id DESC LIMIT 1");
    q.addBindValue(id); q.addBindValue(QString::fromLatin1(kindName(kind)));
    if (!q.exec() || !q.next()) return false;
    out->content = q.value(0).toString();
    out->source = q.value(1).toString();
    return true;
}

bool AnalysisRepository::insert(qint64 id, Kind kind, const QString& text, const QString& source, const QString& model, QString* error)
{
    QSqlDatabase db = m_db.handle();
    auto fail = [&](const QString& m) { if (error) *error = m; db.rollback(); return false; };
    if (!db.transaction()) { if (error) *error = db.lastError().text(); return false; }
    QSqlQuery q(db);
    q.prepare("UPDATE analyses SET is_current=0 WHERE recording_id=? AND kind=?");
    q.addBindValue(id); q.addBindValue(QString::fromLatin1(kindName(kind)));
    if (!q.exec()) return fail(q.lastError().text());
    q.prepare("INSERT INTO analyses(recording_id,kind,content,source,model,created_at,is_current) VALUES(?,?,?,?,?,?,1)");
    q.addBindValue(id); q.addBindValue(QString::fromLatin1(kindName(kind))); q.addBindValue(sqlText(text));
    q.addBindValue(source); q.addBindValue(sqlText(model)); q.addBindValue(QDateTime::currentSecsSinceEpoch());
    if (!q.exec()) return fail(q.lastError().text());   // also rejects unknown recordings (foreign key)
    return db.commit() || fail(db.lastError().text());
}

bool AnalysisRepository::saveAi(qint64 id, Kind kind, const QString& text, const QString& model, bool* keptUser, QString* error)
{
    Entry cur;
    if (keptUser) *keptUser = false;
    if (current(id, kind, &cur) && cur.isUser()) {
        if (keptUser) *keptUser = true;
        return true;
    }
    return insert(id, kind, text, "ai-extractive", model, error);
}

bool AnalysisRepository::saveUser(qint64 id, Kind kind, const QString& text, QString* error)
{
    return insert(id, kind, text, "user", QString(), error);
}

bool AnalysisRepository::clearAi(qint64 id, Kind kind)
{
    QSqlQuery q(m_db.handle());
    q.prepare("DELETE FROM analyses WHERE recording_id=? AND kind=? AND source<>'user'");
    q.addBindValue(id); q.addBindValue(QString::fromLatin1(kindName(kind)));
    return q.exec();
}

bool AnalysisRepository::revertToAi(qint64 id, Kind kind)
{
    QSqlDatabase db = m_db.handle();
    if (!db.transaction()) return false;
    QSqlQuery q(db);
    q.prepare("DELETE FROM analyses WHERE recording_id=? AND kind=? AND source='user'");
    q.addBindValue(id); q.addBindValue(QString::fromLatin1(kindName(kind)));
    bool ok = q.exec();
    if (ok) {   // newest remaining AI row becomes current again
        q.prepare("UPDATE analyses SET is_current=1 WHERE id=(SELECT max(id) FROM analyses WHERE recording_id=? AND kind=?)");
        q.addBindValue(id); q.addBindValue(QString::fromLatin1(kindName(kind)));
        ok = q.exec();
    }
    if (!ok) { db.rollback(); return false; }
    return db.commit();
}

} // namespace ake
