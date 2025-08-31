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

static ValkeyModuleType *DataTieringType;

static int filterCallback(ValkeyModuleCtx *ctx, ValkeyModuleKey *key, int flags) {
    VALKEYMODULE_NOT_USED(flags);
    int type = ValkeyModule_KeyType(key);
    if (type != VALKEYMODULE_KEYTYPE_MODULE || ValkeyModule_ModuleTypeGetType(key) != DataTieringType) {
        return VALKEYMODULE_ERR;
    }

    ValkeyModuleString *newele = ValkeyModule_CreateStringFromLongLong(ctx, 10010);

    int ret = ValkeyModule_StringSet(key, newele);
    if (ret != VALKEYMODULE_OK) {
        ValkeyModule_Log(ctx, "err", "ValkeyModule_StringSet failed");
    } else {
        ValkeyModule_Log(ctx, "info", "ValkeyModule_StringSet success");
    }
    return VALKEYMODULE_OK;
}

void cronLoopCallback(ValkeyModuleCtx *ctx, ValkeyModuleEvent e, uint64_t sub, void *data) {
    VALKEYMODULE_NOT_USED(e);
    VALKEYMODULE_NOT_USED(sub);
    VALKEYMODULE_NOT_USED(data);
    if (ValkeyModule_DbSize(ctx) <= 0) {
        return;
    }

    ValkeyModuleString *key = ValkeyModule_RandomKey(ctx);
    if (key == NULL) {
        return;
    }
    size_t strlen;
    const char *key_str = ValkeyModule_StringPtrLen(key, &strlen);

    ValkeyModuleKey *kp = ValkeyModule_OpenKey(ctx, key, VALKEYMODULE_READ | VALKEYMODULE_WRITE);
    int type = ValkeyModule_KeyType(kp);
    if (type == VALKEYMODULE_KEYTYPE_MODULE && ValkeyModule_ModuleTypeGetType(kp) == DataTieringType) {
        return;
    }

    ValkeyModule_Log(ctx, "info", "cronLoopCallback evict key: %s", key_str);

    ValkeyModule_ModuleTypeSetValue(kp, DataTieringType, NULL);
    ValkeyModule_CloseKey(kp);
}

/* ========================== "datatieringtype" type methods ======================= */

void *DataTieringTypeRdbLoad(ValkeyModuleIO *rdb, int encver) {
    VALKEYMODULE_NOT_USED(rdb);
    VALKEYMODULE_NOT_USED(encver);
    ValkeyModule_Assert(0);
}

void DataTieringTypeRdbSave(ValkeyModuleIO *rdb, void *value) {
    VALKEYMODULE_NOT_USED(rdb);
    VALKEYMODULE_NOT_USED(value);
    ValkeyModule_Assert(0);
}

size_t DataTieringTypeMemUsage(ValkeyModuleKeyOptCtx *ctx, const void *value, size_t sample_size) {
    VALKEYMODULE_NOT_USED(ctx);
    VALKEYMODULE_NOT_USED(value);
    VALKEYMODULE_NOT_USED(sample_size);
    return 0;
}

void DataTieringTypeFree(void *value) {
    VALKEYMODULE_NOT_USED(value);
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    VALKEYMODULE_NOT_USED(argc);

    if (ValkeyModule_Init(ctx, "data-tiering", 1, VALKEYMODULE_APIVER_1)
        == VALKEYMODULE_ERR) return VALKEYMODULE_ERR;

    ValkeyModuleTypeMethods tm = {
        .version = VALKEYMODULE_TYPE_METHOD_VERSION,
        .rdb_load = DataTieringTypeRdbLoad,
        .rdb_save = DataTieringTypeRdbSave,
        .mem_usage2 = DataTieringTypeMemUsage,
        .free = DataTieringTypeFree,
    };

    DataTieringType = ValkeyModule_CreateDataType(ctx, "datatier-", 0, &tm);
    if (DataTieringType == NULL) {
        ValkeyModule_Log(ctx, "err", "ValkeyModule CreateDataType failed");
        return VALKEYMODULE_ERR;
    }

    ValkeyModuleDataTieringFilter *filter = ValkeyModule_RegisterDataTieringFilter(ctx, filterCallback, 0);
    if (filter == NULL) {
        ValkeyModule_Log(ctx, "err", "ValkeyModule RegisterDataTieringFilter failed");
        return VALKEYMODULE_ERR;
    }

    ValkeyModule_SubscribeToServerEvent(ctx, ValkeyModuleEvent_CronLoop, cronLoopCallback);
    return VALKEYMODULE_OK;
}
