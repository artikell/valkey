/* Module designed to test the modules subsystem.
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (c) 2016, Redis Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "valkeymodule.h"
#include <string.h>
#include <stdlib.h>

static int filterCallback(ValkeyModuleCtx *ctx, ValkeyModuleKey *key) {
    size_t strlen;
    const ValkeyModuleString *key_name = ValkeyModule_GetKeyNameFromModuleKey(key);
    const char *str = ValkeyModule_StringPtrLen(key_name, &strlen);
    ValkeyModule_Log(ctx, "info", "key: %s, strlen: %ld", str, strlen);
    ValkeyModuleString *newele = ValkeyModule_CreateStringFromLongLong(ctx, 10010);
    const char *newele_str = ValkeyModule_StringPtrLen(newele, &strlen);
    ValkeyModule_Log(ctx, "info", "newele: %s", newele_str);
    int ret = ValkeyModule_StringSet(key, newele);
    if (ret != VALKEYMODULE_OK) {
        ValkeyModule_Log(ctx, "err", "ValkeyModule_StringSet failed");
    } else {
        ValkeyModule_Log(ctx, "info", "ValkeyModule_StringSet success");
    }
    return 0;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "data-tiering", 1, VALKEYMODULE_APIVER_1)
        == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;
    
    ValkeyModuleDataTieringFilter *filter = ValkeyModule_RegisterDataTieringFilter(ctx, filterCallback, 0);
    if (filter == NULL) {
        return VALKEYMODULE_ERR;
    }

    return VALKEYMODULE_OK;
}
