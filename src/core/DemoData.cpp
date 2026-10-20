#include "core/DemoData.h"

#include <QDateTime>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

#include "core/Database.h"

namespace ake {

namespace {
struct Demo {
    const char* file;
    const char* title; // empty = not analyzed yet
    const char* summary;
    const char* tags; // space separated
    const char* date;
    int durationSec;
    const char* status;
    int favorite;
    const char* error;
};

const Demo kDemo[] = {
    { "meetings/docker.m4a", "Docker移行会議", "Docker ComposeからPodman移行について議論。rootless運用とsystemd連携の課題、段階移行の方針を確認した。", "docker linux podman", "2026-09-12T14:03:00", 2530, "done", 1, "" },
    { "study/podman3.mp3", "Podman勉強会 #3", "rootlessコンテナの仕組みと制約を整理。ネットワーク、ボリューム権限、Quadletによる自動起動の紹介。", "podman systemd 勉強会", "2026-08-30T19:30:00", 3922, "done", 0, "" },
    { "meetings/planning.wav", "新規サービス企画ミーティング", "ターゲット層の再定義と価格帯の議論。MVPの範囲を3機能に絞ることで合意した。", "企画 価格 MVP", "2026-08-21T10:00:00", 3524, "done", 0, "" },
    { "interviews/backend2.flac", "採用面接 バックエンド職 #2", "C++とSQLiteの経験についての質疑。設計判断の説明が明確で、技術面は高評価。", "採用 面接 C++", "2026-08-18T15:10:00", 2102, "done", 0, "" },
    { "podcasts/ep87.ogg", "技術ポッドキャスト Ep.87", "Rust製ツールチェーンの普及とビルド時間の最適化について。キャッシュ戦略が話題の中心。", "rust ビルド podcast", "2026-08-10T07:45:00", 2899, "done", 0, "" },
    { "interviews/interview_04.mp3", "", "", "", "2026-08-02T13:20:00", 4325, "transcribing", 0, "" },
    { "memos/memo0731.m4a", "ボイスメモ 0731", "週次レビューの準備メモ。未解決タスクの洗い出しと優先順位付けについて。", "レビュー タスク", "2026-07-31T22:14:00", 408, "done", 0, "" },
    { "broken/broken_rec.flac", "", "", "", "2026-07-28T09:00:00", 0, "error", 0, "デコードに失敗しました（未対応コーデック or ファイル破損）" },
    { "memos/queued_note.wav", "", "", "", "2026-07-27T18:00:00", 190, "queued", 0, "" },
};
}

bool seedDemoData(Database& db)
{
    QSqlDatabase d = db.handle();
    QSqlQuery q(d);
    if (!d.transaction()) return false;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    bool ok = q.exec("INSERT OR IGNORE INTO folders(id,path) VALUES(1,'/demo/recordings')");
    for (const Demo& m : kDemo) {
        if (!ok) break;
        const qint64 when = QDateTime::fromString(QString::fromLatin1(m.date), Qt::ISODate).toSecsSinceEpoch();
        q.prepare("INSERT INTO recordings(folder_id,rel_path,abs_path,duration_ms,recorded_at,status,favorite,error_msg,format,created_at,updated_at)"
                  " VALUES(1,?,?,?,?,?,?,?,?,?,?)");
        const QString rel = QString::fromUtf8(m.file);
        q.addBindValue(rel); q.addBindValue("/demo/recordings/" + rel);
        q.addBindValue(m.durationSec * 1000); q.addBindValue(when); q.addBindValue(QString::fromLatin1(m.status));
        q.addBindValue(m.favorite); q.addBindValue(QString::fromUtf8(m.error));
        q.addBindValue(rel.section('.', -1)); q.addBindValue(now); q.addBindValue(now);
        ok = q.exec();
        if (!ok) break;
        const qint64 rid = q.lastInsertId().toLongLong();
        if (*m.title) {
            for (auto kv : { std::pair<const char*, const char*>{ "title", m.title }, { "summary", m.summary } }) {
                q.prepare("INSERT INTO analyses(recording_id,kind,content,source,created_at) VALUES(?,?,?,'ai-extractive',?)");
                q.addBindValue(rid); q.addBindValue(QString::fromLatin1(kv.first));
                q.addBindValue(QString::fromUtf8(kv.second)); q.addBindValue(now);
                ok = ok && q.exec();
            }
            double score = 1.0;
            for (const QString& tag : QString::fromUtf8(m.tags).split(' ', Qt::SkipEmptyParts)) {
                q.prepare("INSERT OR IGNORE INTO keywords(name,normalized) VALUES(?,?)");
                q.addBindValue(tag); q.addBindValue(tag.toLower());
                ok = ok && q.exec();
                q.prepare("INSERT INTO recording_keywords(recording_id,keyword_id,score) "
                          "SELECT ?, id, ? FROM keywords WHERE name=?");
                q.addBindValue(rid); q.addBindValue(score); q.addBindValue(tag);
                ok = ok && q.exec();
                score -= 0.1;
            }
        }
        if (QString::fromLatin1(m.status) == "transcribing") {
            q.prepare("INSERT INTO jobs(recording_id,stage,state,progress,updated_at) VALUES(?,'transcribe','running',0.62,?)");
            q.addBindValue(rid); q.addBindValue(now);
            ok = ok && q.exec();
        }
    }
    if (!ok) { d.rollback(); return false; }
    return d.commit();
}

} // namespace ake
