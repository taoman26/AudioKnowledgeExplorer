#pragma once
#include <QString>

namespace ake {

// Where the app keeps its own files (library.db, models/). One place, so every platform follows its own convention.
namespace AppPaths {
// Linux: ~/.local/share/AudioKnowledgeExplorer. Haiku: ~/config/settings/AudioKnowledgeExplorer (Qt's AppDataLocation is
// /boot/system/cache there, which is for disposable files, not for a user's library).
QString dataDirectory();
inline QString modelDirectory() { return dataDirectory() + "/models"; }
inline QString defaultDatabasePath() { return dataDirectory() + "/library.db"; }
} // namespace AppPaths

} // namespace ake
