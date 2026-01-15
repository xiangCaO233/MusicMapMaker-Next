#ifndef MMM_GLOBDEFS_H
#define MMM_GLOBDEFS_H

#include "colorful-log.h"

namespace MMM
{
constexpr int EXIT_NORMAL = 0;
struct RTIILogger {
    RTIILogger() { XLogger::init("MMM"); }
    ~RTIILogger() { XLogger::shutdown(); }
};

inline RTIILogger rtiiLogger;

}  // namespace MMM

#endif  // !MMM_GLOBDEFS_H
