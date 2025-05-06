#ifdef USE_LTTNG

#undef LTTNG_UST_TRACEPOINT_PROVIDER
#define LTTNG_UST_TRACEPOINT_PROVIDER valkey_commands

#undef LTTNG_UST_TRACEPOINT_INCLUDE
#define LTTNG_UST_TRACEPOINT_INCLUDE "./trace_commands.h"

#if !defined(__VALKEY_TRACE_COMMANDS_H__) || defined(LTTNG_UST_TRACEPOINT_HEADER_MULTI_READ)
#define __VALKEY_TRACE_COMMANDS_H__

#include <lttng/tracepoint.h>

LTTNG_UST_TRACEPOINT_EVENT(
	/* Tracepoint provider name */
	valkey_commands,

	/* Tracepoint name */
	command_call,

	/* Input arguments */
	LTTNG_UST_TP_ARGS(
		const char *, prot,
		const char *, conn,
		const char *, name,
		uint64_t, duration
	),

	/* Output event fields */
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_string(prot, prot)
		lttng_ust_field_string(conn, conn)
		lttng_ust_field_string(name, name)
		lttng_ust_field_integer(uint64_t, duration, duration)
	)
)

#define valkey_commands_trace(...) lttng_ust_tracepoint(__VA_ARGS__)

#endif /* __VALKEY_TRACE_COMMANDS_H__ */

#include <lttng/tracepoint-event.h>

#else /* USE_LTTNG */

#ifndef __VALKEY_TRACE_COMMANDS_H__
#define __VALKEY_TRACE_COMMANDS_H__

/* avoid compiler warning on empty source file */
static inline void __valkey_commands_trace(void) {
}

#define valkey_commands_trace(...) \
    do {                           \
    } while (0)

#endif /* __VALKEY_TRACE_COMMANDS_H__ */

#endif /* USE_LTTNG */
