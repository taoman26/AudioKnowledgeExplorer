#pragma once
#include <QString>

namespace ake {

// QSqlQuery binds a null QString as SQL NULL, which violates our `TEXT NOT NULL DEFAULT ''` columns.
// Wrap every optional text value that goes into such a column.
inline QString sqlText(const QString& s) { return s.isNull() ? QStringLiteral("") : s; }

} // namespace ake
