/* ==========================================================================
 * trace.c - support tracing for transport layer.
 * --------------------------------------------------------------------------
 * Copyright (C) 2025  zhiqiang li <lizhiqiang.sf@bytedance.com>
 *
 * This work is licensed under BSD 3-Clause, License 1 of the COPYING file in
 * the top-level directory.
 * ==========================================================================
 */
/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "trace.h"

struct valkeyTraceEvents trace_events = {0};
