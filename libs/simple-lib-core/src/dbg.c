// dbg - process-wide log sink and optional per-stage logger timing.
#include "dbg.h"
#include <sys/sys_time.h>

LogCallback logCallback = 0;
const char *debugLogPath = DBG_LOG;
volatile int logTimingEnabled = 0;

static volatile uint32_t logTimingCount[DBG_LOG_TIMING_STAGES];
static volatile uint32_t logTimingTotalUs[DBG_LOG_TIMING_STAGES];
static volatile uint32_t logTimingMaxUs[DBG_LOG_TIMING_STAGES];

void setDebugLogPath(const char *path)
{
   if (path && path[0]) debugLogPath = path;
}

void setLogTimingEnabled(int on)
{
   logTimingEnabled = on != 0;
}

uint64_t dbgLogTimeUs(void)
{
   return sys_time_get_system_time();
}

void dbgRecordLogTiming(unsigned stage, uint32_t elapsedUs)
{
   if (stage >= DBG_LOG_TIMING_STAGES) return;
   __sync_fetch_and_add(&logTimingCount[stage], 1);
   __sync_fetch_and_add(&logTimingTotalUs[stage], elapsedUs);
   uint32_t previous;
   do {
      previous = logTimingMaxUs[stage];
      if (elapsedUs <= previous) break;
   } while (!__sync_bool_compare_and_swap(&logTimingMaxUs[stage], previous, elapsedUs));
}

void dbgTakeLogTimingStats(DbgLogTimingStats *out)
{
   if (!out) return;
   for (unsigned i = 0; i < DBG_LOG_TIMING_STAGES; i++) {
      out->count[i] = __sync_lock_test_and_set(&logTimingCount[i], 0);
      out->totalUs[i] = __sync_lock_test_and_set(&logTimingTotalUs[i], 0);
      out->maxUs[i] = __sync_lock_test_and_set(&logTimingMaxUs[i], 0);
   }
}

int logDetailed = 0;
