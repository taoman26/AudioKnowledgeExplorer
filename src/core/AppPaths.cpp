#include "core/AppPaths.h"

#include <QStandardPaths>

namespace ake::AppPaths {

QString dataDirectory()
{
#ifdef __HAIKU__
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
#else
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
#endif
}

} // namespace ake::AppPaths
