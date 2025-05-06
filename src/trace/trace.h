#if !defined(__VALKEY_TRACE_H__)
#define __VALKEY_TRACE_H__

#include "trace_db.h"
#include "trace_cluster.h"
#include "trace_sys.h"
#include "trace_aof.h"
#include "trace_server.h"
#include "trace_commands.h"

typedef struct valkeyTraceMask {
    unsigned aof : 1;
    unsigned server : 1;
    unsigned cluster : 1;
    unsigned sys : 1;
    unsigned db : 1;
    unsigned commands : 1;
    unsigned reserved : 1;
} valkeyTraceMask;

extern struct valkeyTraceMask trace_mask;

#ifdef USE_LTTNG
#define latencyTraceIfNeeded(type, event, var) \
    if (trace_mask.type) valkey_##type##_trace(valkey_##type, "latency", (event), (var));
#else
#define latencyTraceIfNeeded(type, event, var) \
    do {                                            \
    } while (0)
#endif

#endif /* __VALKEY_TRACE_H__ */
