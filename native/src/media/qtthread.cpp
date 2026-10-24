#include "qtthread.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace vn {

void lowPowerThread() {
#ifdef _WIN32
    struct { ULONG Version, ControlMask, StateMask; } pt{1, 1, 1};   // THREAD_POWER_THROTTLING_STATE, EXECUTION_SPEED
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &pt, sizeof pt);
    MEMORY_PRIORITY_INFORMATION mp{MEMORY_PRIORITY_LOW};
    SetThreadInformation(GetCurrentThread(), ThreadMemoryPriority, &mp, sizeof mp);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

}
