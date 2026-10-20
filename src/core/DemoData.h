#pragma once

namespace ake {

class Database;

// Inserts clearly fake recordings (title/summary/keywords) matching the design mock-ups.
// Used only for UI verification and demos; never touches real files.
bool seedDemoData(Database& db);

} // namespace ake
