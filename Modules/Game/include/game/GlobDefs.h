#ifndef MMM_GLOBDEFS_H
#define MMM_GLOBDEFS_H

#include "log/colorful-log.h"

namespace MMM
{
constexpr int EXIT_NORMAL          = 0;
constexpr int EXIT_WINDOW_EXEPTION = 1;
struct RTIILogger {
    RTIILogger() { XLogger::init("MMM"); }
    ~RTIILogger() { XLogger::shutdown(); }
};

inline RTIILogger rtiiLogger;

}  // namespace MMM

#endif  // !MMM_GLOBDEFS_H
