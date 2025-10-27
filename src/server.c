/*
 * Copyright (c) 2009-2016, Redis Ltd.
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

#include "server.h"
#include "monotonic.h"
#include "bio.h"
#include "mt19937-64.h"
#include "functions.h"
#include "hdr_histogram.h"
#include "fmtargs.h"
#include "sds.h"

#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <sys/utsname.h>
#include <locale.h>
#include <sys/socket.h>

#ifdef __linux__
#include <sys/mman.h>
#endif

#if defined(HAVE_SYSCTL_KIPC_SOMAXCONN) || defined(HAVE_SYSCTL_KERN_SOMAXCONN)
#include <sys/sysctl.h>
#endif

#ifdef __GNUC__
#define GNUC_VERSION_STR STRINGIFY(__GNUC__) "." STRINGIFY(__GNUC_MINOR__) "." STRINGIFY(__GNUC_PATCHLEVEL__)
#else
#define GNUC_VERSION_STR "0.0.0"
#endif

/* Our shared "common" objects */

struct sharedObjectsStruct shared;

/* Global vars that are actually used as constants. The following double
 * values are used for double on-disk serialization, and are initialized
 * at runtime to avoid strange compiler optimizations. */

double R_Zero, R_PosInf, R_NegInf, R_Nan;

/*================================= Globals ================================= */

/* Global vars */
struct valkeyServer server; /* Server global state */

/*============================ Internal prototypes ========================== */

static inline int isShutdownInitiated(void);
int finishShutdown(void);
const char *replstateToString(int replstate);

/*============================ Utility functions ============================ */

/* This macro tells if we are in the context of loading an AOF. */
#define isAOFLoadingContext() ((server.current_client && server.current_client->id == CLIENT_ID_AOF) ? 1 : 0)

/* We use a private localtime implementation which is fork-safe. The logging
 * function of the server may be called from other threads. */
void nolocks_localtime(struct tm *tmp, time_t t, time_t tz, int dst);

/* Low level logging. To use only for very big messages, otherwise
 * serverLog() is to prefer. */
void serverLogRaw(int level, const char *msg) {
    const int syslogLevelMap[] = {LOG_DEBUG, LOG_INFO, LOG_NOTICE, LOG_WARNING};
    const char *c = ".-*#";
    FILE *fp;
    char buf[64];
    int rawmode = (level & LL_RAW);
    int log_to_stdout = server.logfile[0] == '\0';

    level &= 0xff; /* clear flags */
    if (level < server.verbosity) return;

    /* We open and close the log file in every call to support log rotation.
     * This allows external processes to move or truncate the log file without
     * disrupting logging. */
    fp = log_to_stdout ? stdout : fopen(server.logfile, "a");
    if (!fp) return;

    if (rawmode) {
        fprintf(fp, "%s", msg);
    } else {
        int off;
        struct timeval tv;
        int role_char;
        int daylight_active = atomic_load_explicit(&server.daylight_active, memory_order_relaxed);

        gettimeofday(&tv, NULL);
        struct tm tm;
        nolocks_localtime(&tm, tv.tv_sec, server.timezone, daylight_active);
        off = strftime(buf, sizeof(buf), "%d %b %Y %H:%M:%S.", &tm);
        snprintf(buf + off, sizeof(buf) - off, "%03d", (int)tv.tv_usec / 1000);
        role_char = 'C'; /* RDB / AOF writing child. */
        fprintf(fp, "%d:%c %s %c %s\n", (int)getpid(), role_char, buf, c[level], msg);
    }
    fflush(fp);

    if (!log_to_stdout) fclose(fp);
}

/* Like serverLogRaw() but with printf-alike support. This is the function that
 * is used across the code. The raw version is only used in order to dump
 * the INFO output on crash. */
void _serverLog(int level, const char *fmt, ...) {
    va_list ap;
    char msg[LOG_MAX_LEN];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    serverLogRaw(level, msg);
}

/* Low level logging from signal handler. Should be used with pre-formatted strings.
   See serverLogFromHandler. */
void serverLogRawFromHandler(int level, const char *msg) {
    int fd;
    int log_to_stdout = server.logfile[0] == '\0';
    char buf[64];

    fd = log_to_stdout ? STDOUT_FILENO : open(server.logfile, O_APPEND | O_CREAT | O_WRONLY, 0644);
    if (fd == -1) return;
    if (level & LL_RAW) {
        if (write(fd, msg, strlen(msg)) == -1) goto err;
    } else {
        ll2string(buf, sizeof(buf), getpid());
        if (write(fd, buf, strlen(buf)) == -1) goto err;
        if (write(fd, ":signal-handler (", 17) == -1) goto err;
        ll2string(buf, sizeof(buf), time(NULL));
        if (write(fd, buf, strlen(buf)) == -1) goto err;
        if (write(fd, ") ", 2) == -1) goto err;
        if (write(fd, msg, strlen(msg)) == -1) goto err;
        if (write(fd, "\n", 1) == -1) goto err;
    }
err:
    if (!log_to_stdout) close(fd);
}

/* An async-signal-safe version of serverLog. if LL_RAW is not included in level flags,
 * The message format is: <pid>:signal-handler (<time>) <msg> \n
 * with LL_RAW flag only the msg is printed (with no new line at the end)
 *
 * We actually use this only for signals that are not fatal from the point
 * of view of the server. Signals that are going to kill the server anyway and
 * where we need printf-alike features are served by serverLog(). */
void serverLogFromHandler(int level, const char *fmt, ...) {
    va_list ap;
    char msg[LOG_MAX_LEN];

    va_start(ap, fmt);
    vsnprintf_async_signal_safe(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    serverLogRawFromHandler(level, msg);
}

/* Return the UNIX time in microseconds */
long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec) * 1000000;
    ust += tv.tv_usec;
    return ust;
}

/* Return the UNIX time in milliseconds */
mstime_t mstime(void) {
    return ustime() / 1000;
}

/* Return the command time snapshot in milliseconds.
 * The time the command started is the logical time it runs,
 * and all the time readings during the execution time should
 * reflect the same time.
 * More details can be found in the comments below. */
mstime_t commandTimeSnapshot(void) {
    /* When we are in the middle of a command execution, we want to use a
     * reference time that does not change: in that case we just use the
     * cached time, that we update before each call in the call() function.
     * This way we avoid that commands such as RPOPLPUSH or similar, that
     * may re-open the same key multiple times, can invalidate an already
     * open object in a next call, if the next call will see the key expired,
     * while the first did not.
     * This is specifically important in the context of scripts, where we
     * pretend that time freezes. This way a key can expire only the first time
     * it is accessed and not in the middle of the script execution, making
     * propagation to replicas / AOF consistent. See issue #1525 for more info.
     * Note that we cannot use the cached server.mstime because it can change
     * in processEventsWhileBlocked etc. */
    return server.cmd_time_snapshot;
}

/* After an RDB dump or AOF rewrite we exit from children using _exit() instead of
 * exit(), because the latter may interact with the same file objects used by
 * the parent process. However if we are testing the coverage normal exit() is
 * used in order to obtain the right coverage information. */
void exitFromChild(int retcode) {
#ifdef COVERAGE_TEST
    exit(retcode);
#else
    _exit(retcode);
#endif
}

/*====================== Hash table type implementation  ==================== */

/* This is a hash table type that uses the SDS dynamic strings library as
 * keys and Objects as values (Objects can hold SDS strings,
 * lists, sets). */

void dictVanillaFree(dict *d, void *val) {
    UNUSED(d);
    zfree(val);
}

void dictListDestructor(dict *d, void *val) {
    UNUSED(d);
    listRelease((list *)val);
}

void dictDictDestructor(dict *d, void *val) {
    UNUSED(d);
    dictRelease((dict *)val);
}

int dictSdsKeyCompare(dict *d, const void *key1, const void *key2) {
    int l1, l2;
    UNUSED(d);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

size_t dictSdsEmbedKey(unsigned char *buf, size_t buf_len, const void *key, uint8_t *key_offset) {
    return sdscopytobuffer(buf, buf_len, (sds)key, key_offset);
}

/* A case insensitive version used for the command lookup table and other
 * places where case insensitive non binary-safe comparison is needed. */
int dictSdsKeyCaseCompare(dict *d, const void *key1, const void *key2) {
    UNUSED(d);
    return strcasecmp(key1, key2) == 0;
}

void dictObjectDestructor(dict *d, void *val) {
    UNUSED(d);
    if (val == NULL) return; /* Lazy freeing will set value to NULL. */
    decrRefCount(val);
}

void dictSdsDestructor(dict *d, void *val) {
    UNUSED(d);
    sdsfree(val);
}

void *dictSdsDup(dict *d, const void *key) {
    UNUSED(d);
    return sdsdup((const sds)key);
}

int dictObjKeyCompare(dict *d, const void *key1, const void *key2) {
    const robj *o1 = key1, *o2 = key2;
    return dictSdsKeyCompare(d, o1->ptr, o2->ptr);
}

uint64_t dictObjHash(const void *key) {
    const robj *o = key;
    return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
}

uint64_t dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char *)key, sdslen((char *)key));
}

uint64_t dictSdsCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char *)key, sdslen((char *)key));
}

/* Dict hash function for null terminated string */
uint64_t dictCStrHash(const void *key) {
    return dictGenHashFunction((unsigned char *)key, strlen((char *)key));
}

/* Dict hash function for null terminated string */
uint64_t dictCStrCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char *)key, strlen((char *)key));
}

/* Dict hash function for client */
uint64_t dictClientHash(const void *key) {
    return ((client *)key)->id;
}

/* Dict compare function for client */
int dictClientKeyCompare(dict *d, const void *key1, const void *key2) {
    UNUSED(d);
    return ((client *)key1)->id == ((client *)key2)->id;
}

/* Dict compare function for null terminated string */
int dictCStrKeyCompare(dict *d, const void *key1, const void *key2) {
    int l1, l2;
    UNUSED(d);

    l1 = strlen((char *)key1);
    l2 = strlen((char *)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

/* Dict case insensitive compare function for null terminated string */
int dictCStrKeyCaseCompare(dict *d, const void *key1, const void *key2) {
    UNUSED(d);
    return strcasecmp(key1, key2) == 0;
}

int dictEncObjKeyCompare(dict *d, const void *key1, const void *key2) {
    robj *o1 = (robj *)key1, *o2 = (robj *)key2;
    int cmp;

    if (o1->encoding == OBJ_ENCODING_INT && o2->encoding == OBJ_ENCODING_INT) return o1->ptr == o2->ptr;

    /* Due to OBJ_STATIC_REFCOUNT, we avoid calling getDecodedObject() without
     * good reasons, because it would incrRefCount() the object, which
     * is invalid. So we check to make sure dictFind() works with static
     * objects as well. */
    if (o1->refcount != OBJ_STATIC_REFCOUNT) o1 = getDecodedObject(o1);
    if (o2->refcount != OBJ_STATIC_REFCOUNT) o2 = getDecodedObject(o2);
    cmp = dictSdsKeyCompare(d, o1->ptr, o2->ptr);
    if (o1->refcount != OBJ_STATIC_REFCOUNT) decrRefCount(o1);
    if (o2->refcount != OBJ_STATIC_REFCOUNT) decrRefCount(o2);
    return cmp;
}

uint64_t dictEncObjHash(const void *key) {
    robj *o = (robj *)key;

    if (sdsEncodedObject(o)) {
        return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
    } else if (o->encoding == OBJ_ENCODING_INT) {
        char buf[32];
        int len;

        len = ll2string(buf, 32, (long)o->ptr);
        return dictGenHashFunction((unsigned char *)buf, len);
    } else {
        serverPanic("Unknown string encoding");
    }
}

/* Return 1 if currently we allow dict to expand. Dict may allocate huge
 * memory to contain hash buckets when dict expands, that may lead the server to
 * reject user's requests or evict some keys, we can stop dict to expand
 * provisionally if used memory will be over maxmemory after dict expands,
 * but to guarantee the performance of the server, we still allow dict to expand
 * if dict load factor exceeds HASHTABLE_MAX_LOAD_FACTOR. */
int dictResizeAllowed(size_t moreMem, double usedRatio) {
    if (usedRatio <= HASHTABLE_MAX_LOAD_FACTOR) {
        return !overMaxmemoryAfterAlloc(moreMem);
    } else {
        return 1;
    }
}

/* Generic hash table type where keys are Objects, Values
 * dummy pointers. */
dictType objectKeyPointerValueDictType = {
    dictEncObjHash,       /* hash function */
    NULL,                 /* key dup */
    dictEncObjKeyCompare, /* key compare */
    dictObjectDestructor, /* key destructor */
    NULL,                 /* val destructor */
    NULL                  /* allow to expand */
};

/* Like objectKeyPointerValueDictType(), but values can be destroyed, if
 * not NULL, calling zfree(). */
dictType objectKeyHeapPointerValueDictType = {
    dictEncObjHash,       /* hash function */
    NULL,                 /* key dup */
    dictEncObjKeyCompare, /* key compare */
    dictObjectDestructor, /* key destructor */
    dictVanillaFree,      /* val destructor */
    NULL                  /* allow to expand */
};

/* Set dictionary type. Keys are SDS strings, values are not used. */
dictType setDictType = {
    dictSdsHash,       /* hash function */
    NULL,              /* key dup */
    dictSdsKeyCompare, /* key compare */
    dictSdsDestructor, /* key destructor */
    NULL,              /* val destructor */
    NULL,              /* allow to expand */
    .no_value = 1,     /* no values in this dict */
    .keys_are_odd = 1  /* an SDS string is always an odd pointer */
};

/* Sorted sets hash (note: a skiplist is used in addition to the hash table) */
dictType zsetDictType = {
    dictSdsHash,       /* hash function */
    NULL,              /* key dup */
    dictSdsKeyCompare, /* key compare */
    NULL,              /* Note: SDS string shared & freed by skiplist */
    NULL,              /* val destructor */
    NULL,              /* allow to expand */
};

/* Kvstore->keys, keys are sds strings, vals are Objects. */
dictType kvstoreKeysDictType = {
    dictSdsHash,          /* hash function */
    NULL,                 /* key dup */
    dictSdsKeyCompare,    /* key compare */
    NULL,                 /* key is embedded in the dictEntry and freed internally */
    dictObjectDestructor, /* val destructor */
    dictResizeAllowed,    /* allow to resize */
    kvstoreDictRehashingStarted,
    kvstoreDictRehashingCompleted,
    kvstoreDictMetadataSize,
    .embedKey = dictSdsEmbedKey,
    .embedded_entry = 1,
};

/* Kvstore->expires */
dictType kvstoreExpiresDictType = {
    dictSdsHash,       /* hash function */
    NULL,              /* key dup */
    dictSdsKeyCompare, /* key compare */
    NULL,              /* key destructor */
    NULL,              /* val destructor */
    dictResizeAllowed, /* allow to resize */
    kvstoreDictRehashingStarted,
    kvstoreDictRehashingCompleted,
    kvstoreDictMetadataSize,
};

/* Command table. sds string -> command struct pointer. */
dictType commandTableDictType = {
    dictSdsCaseHash,            /* hash function */
    NULL,                       /* key dup */
    dictSdsKeyCaseCompare,      /* key compare */
    dictSdsDestructor,          /* key destructor */
    NULL,                       /* val destructor */
    NULL,                       /* allow to expand */
    .no_incremental_rehash = 1, /* no incremental rehash as the command table may be accessed from IO threads. */
};

/* Hash type hash table (note that small hashes are represented with listpacks) */
dictType hashDictType = {
    dictSdsHash,       /* hash function */
    NULL,              /* key dup */
    dictSdsKeyCompare, /* key compare */
    dictSdsDestructor, /* key destructor */
    dictSdsDestructor, /* val destructor */
    NULL,              /* allow to expand */
};

/* Dict type without destructor */
dictType sdsReplyDictType = {
    dictSdsHash,       /* hash function */
    NULL,              /* key dup */
    dictSdsKeyCompare, /* key compare */
    NULL,              /* key destructor */
    NULL,              /* val destructor */
    NULL               /* allow to expand */
};

/* Keylist hash table type has unencoded Objects as keys and
 * lists as values. It's used for blocking operations (BLPOP) and to
 * map swapped keys to a list of clients waiting for this keys to be loaded. */
dictType keylistDictType = {
    dictObjHash,          /* hash function */
    NULL,                 /* key dup */
    dictObjKeyCompare,    /* key compare */
    dictObjectDestructor, /* key destructor */
    dictListDestructor,   /* val destructor */
    NULL                  /* allow to expand */
};

/* KeyDict hash table type has unencoded Objects as keys and
 * dicts as values. It's used for PUBSUB command to track clients subscribing the patterns. */
dictType objToDictDictType = {
    dictObjHash,          /* hash function */
    NULL,                 /* key dup */
    dictObjKeyCompare,    /* key compare */
    dictObjectDestructor, /* key destructor */
    dictDictDestructor,   /* val destructor */
    NULL                  /* allow to expand */
};

/* Same as objToDictDictType, added some kvstore callbacks, it's used
 * for PUBSUB command to track clients subscribing the channels. */
dictType kvstoreChannelDictType = {
    dictObjHash,          /* hash function */
    NULL,                 /* key dup */
    dictObjKeyCompare,    /* key compare */
    dictObjectDestructor, /* key destructor */
    dictDictDestructor,   /* val destructor */
    NULL,                 /* allow to expand */
    kvstoreDictRehashingStarted,
    kvstoreDictRehashingCompleted,
    kvstoreDictMetadataSize,
};

/* Modules system dictionary type. Keys are module name,
 * values are pointer to ValkeyModule struct. */
dictType modulesDictType = {
    dictSdsCaseHash,       /* hash function */
    NULL,                  /* key dup */
    dictSdsKeyCaseCompare, /* key compare */
    dictSdsDestructor,     /* key destructor */
    NULL,                  /* val destructor */
    NULL                   /* allow to expand */
};

/* Migrate cache dict type. */
dictType migrateCacheDictType = {
    dictSdsHash,       /* hash function */
    NULL,              /* key dup */
    dictSdsKeyCompare, /* key compare */
    dictSdsDestructor, /* key destructor */
    NULL,              /* val destructor */
    NULL               /* allow to expand */
};

/* Dict for for case-insensitive search using null terminated C strings.
 * The keys stored in dict are sds though. */
dictType stringSetDictType = {
    dictCStrCaseHash,       /* hash function */
    NULL,                   /* key dup */
    dictCStrKeyCaseCompare, /* key compare */
    dictSdsDestructor,      /* key destructor */
    NULL,                   /* val destructor */
    NULL                    /* allow to expand */
};

/* Dict for for case-insensitive search using null terminated C strings.
 * The key and value do not have a destructor. */
dictType externalStringType = {
    dictCStrCaseHash,       /* hash function */
    NULL,                   /* key dup */
    dictCStrKeyCaseCompare, /* key compare */
    NULL,                   /* key destructor */
    NULL,                   /* val destructor */
    NULL                    /* allow to expand */
};

/* Dict for case-insensitive search using sds objects with a zmalloc
 * allocated object as the value. */
dictType sdsHashDictType = {
    dictSdsCaseHash,       /* hash function */
    NULL,                  /* key dup */
    dictSdsKeyCaseCompare, /* key compare */
    dictSdsDestructor,     /* key destructor */
    dictVanillaFree,       /* val destructor */
    NULL                   /* allow to expand */
};

/* Client Set dictionary type. Keys are client, values are not used. */
dictType clientDictType = {
    dictClientHash,       /* hash function */
    NULL,                 /* key dup */
    dictClientKeyCompare, /* key compare */
    .no_value = 1         /* no values in this dict */
};

/* This function is called once a background process of some kind terminates,
 * as we want to avoid resizing the hash tables when there is a child in order
 * to play well with copy-on-write (otherwise when a resize happens lots of
 * memory pages are copied). The goal of this function is to update the ability
 * for dict.c to resize or rehash the tables accordingly to the fact we have an
 * active fork child running. */
void updateDictResizePolicy(void) {
    dictSetResizeEnabled(DICT_RESIZE_ENABLE);
}

/* Returns true when we're inside a long command that yielded to the event loop. */
int isInsideYieldingLongCommand(void) {
    return scriptIsTimedout() || server.busy_module_yield_flags;
}

/* ======================= Cron: called every 100 ms ======================== */
/* The client query buffer is an sds.c string that can end with a lot of
 * free space not used, this function reclaims space if needed.
 *
 * The function always returns 0 as it never terminates the client. */
int clientsCronResizeQueryBuffer(client *c) {
    /* If the client query buffer is NULL, it is using the shared query buffer and there is nothing to do. */
    if (c->querybuf == NULL) return 0;
    size_t querybuf_size = sdsalloc(c->querybuf);
    time_t idletime = server.unixtime - c->last_interaction;

    /* Only resize the query buffer if the buffer is actually wasting at least a
     * few kbytes */
    if (sdsavail(c->querybuf) > 1024 * 4) {
        /* There are two conditions to resize the query buffer: */
        if (idletime > 2) {
            /* 1) Query is idle for a long time. */
            size_t remaining = sdslen(c->querybuf) - c->qb_pos;
            if (!c->flag.primary && !remaining) {
                /* If the client is not a primary and no data is pending,
                 * The client can safely use the shared query buffer in the next read - free the client's querybuf. */
                sdsfree(c->querybuf);
                /* By setting the querybuf to NULL, the client will use the shared query buffer in the next read.
                 * We don't move the client to the shared query buffer immediately, because if we allocated a private
                 * query buffer for the client, it's likely that the client will use it again soon. */
                c->querybuf = NULL;
            } else {
                c->querybuf = sdsRemoveFreeSpace(c->querybuf, 1);
            }
        } else if (querybuf_size > PROTO_RESIZE_THRESHOLD && querybuf_size / 2 > c->querybuf_peak) {
            /* 2) Query buffer is too big for latest peak and is larger than
             *    resize threshold. Trim excess space but only up to a limit,
             *    not below the recent peak and current c->querybuf (which will
             *    be soon get used). If we're in the middle of a bulk then make
             *    sure not to resize to less than the bulk length. */
            size_t resize = sdslen(c->querybuf);
            if (resize < c->querybuf_peak) resize = c->querybuf_peak;
            if (c->bulklen != -1 && resize < (size_t)c->bulklen + 2) resize = c->bulklen + 2;
            c->querybuf = sdsResize(c->querybuf, resize, 1);
        }
    }

    /* Reset the peak again to capture the peak memory usage in the next
     * cycle. */
    c->querybuf_peak = c->querybuf ? sdslen(c->querybuf) : 0;
    /* We reset to either the current used, or currently processed bulk size,
     * which ever is bigger. */
    if (c->bulklen != -1 && (size_t)c->bulklen + 2 > c->querybuf_peak) c->querybuf_peak = c->bulklen + 2;
    return 0;
}

/* The client output buffer can be adjusted to better fit the memory requirements.
 *
 * the logic is:
 * in case the last observed peak size of the buffer equals the buffer size - we double the size
 * in case the last observed peak size of the buffer is less than half the buffer size - we shrink by half.
 * The buffer peak will be reset back to the buffer position every server.reply_buffer_peak_reset_time milliseconds
 * The function always returns 0 as it never terminates the client. */
int clientsCronResizeOutputBuffer(client *c, mstime_t now_ms) {
    if (c->io_write_state != CLIENT_IDLE) return 0;

    size_t new_buffer_size = 0;
    char *oldbuf = NULL;
    const size_t buffer_target_shrink_size = c->buf_usable_size / 2;
    const size_t buffer_target_expand_size = c->buf_usable_size * 2;

    if (buffer_target_shrink_size >= PROTO_REPLY_MIN_BYTES && c->buf_peak < buffer_target_shrink_size) {
        new_buffer_size = max(PROTO_REPLY_MIN_BYTES, c->buf_peak + 1);
        server.stat_reply_buffer_shrinks++;
    } else if (buffer_target_expand_size < PROTO_REPLY_CHUNK_BYTES * 2 && c->buf_peak == c->buf_usable_size) {
        new_buffer_size = min(PROTO_REPLY_CHUNK_BYTES, buffer_target_expand_size);
        server.stat_reply_buffer_expands++;
    }

    serverAssertWithInfo(c, NULL, (!new_buffer_size) || (new_buffer_size >= (size_t)c->bufpos));

    /* reset the peak value each server.reply_buffer_peak_reset_time seconds. in case the client will be idle
     * it will start to shrink.
     */
    if (server.reply_buffer_peak_reset_time >= 0 &&
        now_ms - c->buf_peak_last_reset_time >= server.reply_buffer_peak_reset_time) {
        c->buf_peak = c->bufpos;
        c->buf_peak_last_reset_time = now_ms;
    }

    if (new_buffer_size) {
        oldbuf = c->buf;
        c->buf = zmalloc_usable(new_buffer_size, &c->buf_usable_size);
        memcpy(c->buf, oldbuf, c->bufpos);
        zfree(oldbuf);
    }
    return 0;
}

/* This function is used in order to track clients using the biggest amount
 * of memory in the latest few seconds. This way we can provide such information
 * in the INFO output (clients section), without having to do an O(N) scan for
 * all the clients.
 *
 * This is how it works. We have an array of CLIENTS_PEAK_MEM_USAGE_SLOTS slots
 * where we track, for each, the biggest client output and input buffers we
 * saw in that slot. Every slot corresponds to one of the latest seconds, since
 * the array is indexed by doing UNIXTIME % CLIENTS_PEAK_MEM_USAGE_SLOTS.
 *
 * When we want to know what was recently the peak memory usage, we just scan
 * such few slots searching for the maximum value. */
#define CLIENTS_PEAK_MEM_USAGE_SLOTS 8
size_t ClientsPeakMemInput[CLIENTS_PEAK_MEM_USAGE_SLOTS] = {0};
size_t ClientsPeakMemOutput[CLIENTS_PEAK_MEM_USAGE_SLOTS] = {0};

int clientsCronTrackExpansiveClients(client *c, int time_idx) {
    size_t qb_size = c->querybuf ? sdsAllocSize(c->querybuf) : 0;
    size_t argv_size = c->argv ? zmalloc_size(c->argv) : 0;
    size_t in_usage = qb_size + c->argv_len_sum + argv_size;
    size_t out_usage = getClientOutputBufferMemoryUsage(c);

    /* Track the biggest values observed so far in this slot. */
    if (in_usage > ClientsPeakMemInput[time_idx]) ClientsPeakMemInput[time_idx] = in_usage;
    if (out_usage > ClientsPeakMemOutput[time_idx]) ClientsPeakMemOutput[time_idx] = out_usage;

    return 0; /* This function never terminates the client. */
}

/* All normal clients are placed in one of the "mem usage buckets" according
 * to how much memory they currently use. We use this function to find the
 * appropriate bucket based on a given memory usage value. The algorithm simply
 * does a log2(mem) to ge the bucket. This means, for examples, that if a
 * client's memory usage doubles it's moved up to the next bucket, if it's
 * halved we move it down a bucket.
 * For more details see CLIENT_MEM_USAGE_BUCKETS documentation in server.h. */
static inline clientMemUsageBucket *getMemUsageBucket(size_t mem) {
    int size_in_bits = 8 * (int)sizeof(mem);
    int clz = mem > 0 ? __builtin_clzl(mem) : size_in_bits;
    int bucket_idx = size_in_bits - clz;
    if (bucket_idx > CLIENT_MEM_USAGE_BUCKET_MAX_LOG)
        bucket_idx = CLIENT_MEM_USAGE_BUCKET_MAX_LOG;
    else if (bucket_idx < CLIENT_MEM_USAGE_BUCKET_MIN_LOG)
        bucket_idx = CLIENT_MEM_USAGE_BUCKET_MIN_LOG;
    bucket_idx -= CLIENT_MEM_USAGE_BUCKET_MIN_LOG;
    return &server.client_mem_usage_buckets[bucket_idx];
}

/*
 * This method updates the client memory usage and update the
 * server stats for client type.
 *
 * This method is called from the clientsCron to have updated
 * stats for non CLIENT_TYPE_NORMAL/PUBSUB clients to accurately
 * provide information around clients memory usage.
 *
 * It is also used in updateClientMemUsageAndBucket to have latest
 * client memory usage information to place it into appropriate client memory
 * usage bucket.
 */
void updateClientMemoryUsage(client *c) {
    serverAssert(c->conn);
    size_t mem = getClientMemoryUsage(c, NULL);
    int type = getClientType(c);
    /* Now that we have the memory used by the client, remove the old
     * value from the old category, and add it back. */
    server.stat_clients_type_memory[c->last_memory_type] -= c->last_memory_usage;
    server.stat_clients_type_memory[type] += mem;
    /* Remember what we added and where, to remove it next time. */
    c->last_memory_type = type;
    c->last_memory_usage = mem;
}

int clientEvictionAllowed(client *c) {
    if (server.maxmemory_clients == 0 || c->flag.no_evict || c->flag.fake) {
        return 0;
    }
    serverAssert(c->conn);
    int type = getClientType(c);
    return (type == CLIENT_TYPE_NORMAL || type == CLIENT_TYPE_PUBSUB);
}


/* This function is used to cleanup the client's previously tracked memory usage.
 * This is called during incremental client memory usage tracking as well as
 * used to reset when client to bucket allocation is not required when
 * client eviction is disabled.  */
void removeClientFromMemUsageBucket(client *c, int allow_eviction) {
    if (c->mem_usage_bucket) {
        c->mem_usage_bucket->mem_usage_sum -= c->last_memory_usage;
        /* If this client can't be evicted then remove it from the mem usage
         * buckets */
        if (!allow_eviction) {
            listDelNode(c->mem_usage_bucket->clients, c->mem_usage_bucket_node);
            c->mem_usage_bucket = NULL;
            c->mem_usage_bucket_node = NULL;
        }
    }
}

/* This is called only if explicit clients when something changed their buffers,
 * so we can track clients' memory and enforce clients' maxmemory in real time.
 *
 * This also adds the client to the correct memory usage bucket. Each bucket contains
 * all clients with roughly the same amount of memory. This way we group
 * together clients consuming about the same amount of memory and can quickly
 * free them in case we reach maxmemory-clients (client eviction).
 *
 * Note: This function filters clients of type no-evict, primary or replica regardless
 * of whether the eviction is enabled or not, so the memory usage we get from these
 * types of clients via the INFO command may be out of date.
 *
 * returns 1 if client eviction for this client is allowed, 0 otherwise.
 */
int updateClientMemUsageAndBucket(client *c) {
    int allow_eviction = clientEvictionAllowed(c);
    removeClientFromMemUsageBucket(c, allow_eviction);

    if (!allow_eviction) {
        return 0;
    }

    /* Update client memory usage. */
    updateClientMemoryUsage(c);

    /* Update the client in the mem usage buckets */
    clientMemUsageBucket *bucket = getMemUsageBucket(c->last_memory_usage);
    bucket->mem_usage_sum += c->last_memory_usage;
    if (bucket != c->mem_usage_bucket) {
        if (c->mem_usage_bucket) listDelNode(c->mem_usage_bucket->clients, c->mem_usage_bucket_node);
        c->mem_usage_bucket = bucket;
        listAddNodeTail(bucket->clients, c);
        c->mem_usage_bucket_node = listLast(bucket->clients);
    }
    return 1;
}

/* Return the max samples in the memory usage of clients tracked by
 * the function clientsCronTrackExpansiveClients(). */
void getExpansiveClientsInfo(size_t *in_usage, size_t *out_usage) {
    size_t i = 0, o = 0;
    for (int j = 0; j < CLIENTS_PEAK_MEM_USAGE_SLOTS; j++) {
        if (ClientsPeakMemInput[j] > i) i = ClientsPeakMemInput[j];
        if (ClientsPeakMemOutput[j] > o) o = ClientsPeakMemOutput[j];
    }
    *in_usage = i;
    *out_usage = o;
}

/* This function is called by serverCron() and is used in order to perform
 * operations on clients that are important to perform constantly. For instance
 * we use this function in order to disconnect clients after a timeout, including
 * clients blocked in some blocking command with a non-zero timeout.
 *
 * The function makes some effort to process all the clients every second, even
 * if this cannot be strictly guaranteed, since serverCron() may be called with
 * an actual frequency lower than server.hz in case of latency events like slow
 * commands.
 *
 * It is very important for this function, and the functions it calls, to be
 * very fast: sometimes the server has tens of hundreds of connected clients, and the
 * default server.hz value is 10, so sometimes here we need to process thousands
 * of clients per second, turning this function into a source of latency.
 */
#define CLIENTS_CRON_MIN_ITERATIONS 5
void clientsCron(void) {
    /* Try to process at least numclients/server.hz of clients
     * per call. Since normally (if there are no big latency events) this
     * function is called server.hz times per second, in the average case we
     * process all the clients in 1 second. */
    int numclients = listLength(server.clients);
    int iterations = numclients / server.hz;
    mstime_t now = mstime();

    /* Process at least a few clients while we are at it, even if we need
     * to process less than CLIENTS_CRON_MIN_ITERATIONS to meet our contract
     * of processing each client once per second. */
    if (iterations < CLIENTS_CRON_MIN_ITERATIONS)
        iterations = (numclients < CLIENTS_CRON_MIN_ITERATIONS) ? numclients : CLIENTS_CRON_MIN_ITERATIONS;


    int curr_peak_mem_usage_slot = server.unixtime % CLIENTS_PEAK_MEM_USAGE_SLOTS;
    /* Always zero the next sample, so that when we switch to that second, we'll
     * only register samples that are greater in that second without considering
     * the history of such slot.
     *
     * Note: our index may jump to any random position if serverCron() is not
     * called for some reason with the normal frequency, for instance because
     * some slow command is called taking multiple seconds to execute. In that
     * case our array may end containing data which is potentially older
     * than CLIENTS_PEAK_MEM_USAGE_SLOTS seconds: however this is not a problem
     * since here we want just to track if "recently" there were very expansive
     * clients from the POV of memory usage. */
    int zeroidx = (curr_peak_mem_usage_slot + 1) % CLIENTS_PEAK_MEM_USAGE_SLOTS;
    ClientsPeakMemInput[zeroidx] = 0;
    ClientsPeakMemOutput[zeroidx] = 0;


    while (listLength(server.clients) && iterations--) {
        client *c;
        listNode *head;

        /* Take the current head, process, and then rotate the head to tail.
         * This way we can fairly iterate all clients step by step. */
        head = listFirst(server.clients);
        c = listNodeValue(head);
        listRotateHeadToTail(server.clients);
        if (c->io_read_state != CLIENT_IDLE || c->io_write_state != CLIENT_IDLE) continue;
        /* The following functions do different service checks on the client.
         * The protocol is that they return non-zero if the client was
         * terminated. */
        if (clientsCronHandleTimeout(c, now)) continue;
        if (clientsCronResizeQueryBuffer(c)) continue;
        if (clientsCronResizeOutputBuffer(c, now)) continue;

        if (clientsCronTrackExpansiveClients(c, curr_peak_mem_usage_slot)) continue;

        /* Iterating all the clients in getMemoryOverheadData() is too slow and
         * in turn would make the INFO command too slow. So we perform this
         * computation incrementally and track the (not instantaneous but updated
         * to the second) total memory used by clients using clientsCron() in
         * a more incremental way (depending on server.hz).
         * If client eviction is enabled, update the bucket as well. */
        if (!updateClientMemUsageAndBucket(c)) updateClientMemoryUsage(c);

        if (closeClientOnOutputBufferLimitReached(c, 0)) continue;
    }
}

/* This function handles 'background' operations we are required to do
 * incrementally in the databases, such as active key expiring, resizing,
 * rehashing. */
void databasesCron(void) {
    /* Expire keys by random sampling. Not required for replicas
     * as primary will synthesize DELs for us. */
    if (iAmPrimary()) {
        activeExpireCycle(ACTIVE_EXPIRE_CYCLE_SLOW);
    } else {
        expireReplicaKeys();
    }

    /* We use global counters so if we stop the computation at a given
        * DB we'll be able to start from the successive in the next
        * cron loop iteration. */
    static unsigned int resize_db = 0;
    static unsigned int rehash_db = 0;
    int dbs_per_call = CRON_DBS_PER_CALL;
    int j;

    /* Don't test more DBs than we have. */
    if (dbs_per_call > server.dbnum) dbs_per_call = server.dbnum;

    for (j = 0; j < dbs_per_call; j++) {
        serverDb *db = &server.db[resize_db % server.dbnum];
        kvstoreTryResizeDicts(db->keys, CRON_DICTS_PER_DB);
        kvstoreTryResizeDicts(db->expires, CRON_DICTS_PER_DB);
        resize_db++;
    }

    /* Rehash */
    uint64_t elapsed_us = 0;
    for (j = 0; j < dbs_per_call; j++) {
        serverDb *db = &server.db[rehash_db % server.dbnum];
        elapsed_us += kvstoreIncrementallyRehash(db->keys, INCREMENTAL_REHASHING_THRESHOLD_US - elapsed_us);
        if (elapsed_us >= INCREMENTAL_REHASHING_THRESHOLD_US) break;
        elapsed_us += kvstoreIncrementallyRehash(db->expires, INCREMENTAL_REHASHING_THRESHOLD_US - elapsed_us);
        if (elapsed_us >= INCREMENTAL_REHASHING_THRESHOLD_US) break;
        rehash_db++;
    }
}

static inline void updateCachedTimeWithUs(int update_daylight_info, const long long ustime) {
    server.ustime = ustime;
    server.mstime = server.ustime / 1000;
    server.unixtime = server.mstime / 1000;

    /* To get information about daylight saving time, we need to call
     * localtime_r and cache the result. However calling localtime_r in this
     * context is safe since we will never fork() while here, in the main
     * thread. The logging function will call a thread safe version of
     * localtime that has no locks. */
    if (update_daylight_info) {
        struct tm tm;
        time_t ut = server.unixtime;
        localtime_r(&ut, &tm);
        atomic_store_explicit(&server.daylight_active, tm.tm_isdst, memory_order_relaxed);
    }
}

/* We take a cached value of the unix time in the global state because with
 * virtual memory and aging there is to store the current time in objects at
 * every object access, and accuracy is not needed. To access a global var is
 * a lot faster than calling time(NULL).
 *
 * This function should be fast because it is called at every command execution
 * in call(), so it is possible to decide if to update the daylight saving
 * info or not using the 'update_daylight_info' argument. Normally we update
 * such info only when calling this function from serverCron() but not when
 * calling it from call(). */
void updateCachedTime(int update_daylight_info) {
    const long long us = ustime();
    updateCachedTimeWithUs(update_daylight_info, us);
}

/* Performing required operations in order to enter an execution unit.
 * In general, if we are already inside an execution unit then there is nothing to do,
 * otherwise we need to update cache times so the same cached time will be used all over
 * the execution unit.
 * update_cached_time - if 0, will not update the cached time even if required.
 * us - if not zero, use this time for cached time, otherwise get current time. */
void enterExecutionUnit(int update_cached_time, long long us) {
    if (server.execution_nesting++ == 0 && update_cached_time) {
        if (us == 0) {
            us = ustime();
        }
        updateCachedTimeWithUs(0, us);
        server.cmd_time_snapshot = server.mstime;
    }
}

void exitExecutionUnit(void) {
    --server.execution_nesting;
}

void checkChildrenDone(void) {
}

/* Called from serverCron and cronUpdateMemoryStats to update cached memory metrics. */
void cronUpdateMemoryStats(void) {
    /* Record the max memory used since the server was started. */
    if (zmalloc_used_memory() > server.stat_peak_memory) server.stat_peak_memory = zmalloc_used_memory();

    run_with_period(100) {
        /* Sample the RSS and other metrics here since this is a relatively slow call.
         * We must sample the zmalloc_used at the same time we take the rss, otherwise
         * the frag ratio calculate may be off (ratio of two samples at different times) */
        server.cron_malloc_stats.process_rss = zmalloc_get_rss();
        server.cron_malloc_stats.zmalloc_used = zmalloc_used_memory();
        /* Sampling the allocator info can be slow too.
         * The fragmentation ratio it'll show is potentially more accurate
         * it excludes other RSS pages such as: shared libraries, LUA and other non-zmalloc
         * allocations, and allocator reserved pages that can be pursed (all not actual frag) */
        zmalloc_get_allocator_info(
            &server.cron_malloc_stats.allocator_allocated, &server.cron_malloc_stats.allocator_active,
            &server.cron_malloc_stats.allocator_resident, NULL, &server.cron_malloc_stats.allocator_muzzy,
            &server.cron_malloc_stats.allocator_frag_smallbins_bytes);
        /* in case the allocator isn't providing these stats, fake them so that
         * fragmentation info still shows some (inaccurate metrics) */
        if (!server.cron_malloc_stats.allocator_resident) {
            /* LUA memory isn't part of zmalloc_used, but it is part of the process RSS,
             * so we must deduct it in order to be able to calculate correct
             * "allocator fragmentation" ratio */
            size_t lua_memory = evalMemory();
            server.cron_malloc_stats.allocator_resident = server.cron_malloc_stats.process_rss - lua_memory;
        }
        if (!server.cron_malloc_stats.allocator_active)
            server.cron_malloc_stats.allocator_active = server.cron_malloc_stats.allocator_resident;
        if (!server.cron_malloc_stats.allocator_allocated)
            server.cron_malloc_stats.allocator_allocated = server.cron_malloc_stats.zmalloc_used;
    }
}

/* This is our timer interrupt, called server.hz times per second.
 * Here is where we do a number of things that need to be done asynchronously.
 * For instance:
 *
 * - Active expired keys collection (it is also performed in a lazy way on
 *   lookup).
 * - Software watchdog.
 * - Update some statistic.
 * - Incremental rehashing of the DBs hash tables.
 * - Triggering BGSAVE / AOF rewrite, and handling of terminated children.
 * - Clients timeout of different kinds.
 * - Replication reconnection.
 * - Many more...
 *
 * Everything directly called here will be called server.hz times per second,
 * so in order to throttle execution of things we want to do less frequently
 * a macro is used: run_with_period(milliseconds) { .... }
 */

int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    int j;
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);

    server.hz = server.config_hz;

    /* We have just LRU_BITS bits per object for LRU information.
     * So we use an (eventually wrapping) LRU clock.
     *
     * Note that even if the counter wraps it's not a big problem,
     * everything will still work but some object will appear younger
     * to the server. However for this to happen a given object should never be
     * touched for all the time needed to the counter to wrap, which is
     * not likely.
     *
     * Note that you can change the resolution altering the
     * LRU_CLOCK_RESOLUTION define. */
    server.lruclock = getLRUClock();

    cronUpdateMemoryStats();

    run_with_period(5000) {
        char hmem[64];
        size_t zmalloc_used = zmalloc_used_memory();
        bytesToHuman(hmem, sizeof(hmem), zmalloc_used);

        serverLog(LL_DEBUG, "Total: %lu clients connected (%lu replicas), %zu (%s) bytes in use",
                    listLength(server.clients) - listLength(server.replicas), listLength(server.replicas),
                    zmalloc_used, hmem);
    }

    /* We need to do a few operations on clients asynchronously. */
    clientsCron();

    /* Handle background operations on databases. */
    databasesCron();

    /* Just for the sake of defensive programming, to avoid forgetting to
     * call this function when needed. */
    updateDictResizePolicy();

    /* Clear the paused actions state if needed. */
    updatePausedActions();

    if (moduleCount()) {
        run_with_period(100) modulesCron();
    }

    /* Fire the cron loop modules event. */
    ValkeyModuleCronLoopV1 ei = {VALKEYMODULE_CRON_LOOP_VERSION, server.hz};
    moduleFireServerEvent(VALKEYMODULE_EVENT_CRON_LOOP, 0, &ei);

    server.cronloops++;

    return 1000 / server.hz;
}


void blockingOperationStarts(void) {
    if (!server.blocking_op_nesting++) {
        updateCachedTime(0);
        server.blocked_last_cron = server.mstime;
    }
}

void blockingOperationEnds(void) {
    if (!(--server.blocking_op_nesting)) {
        server.blocked_last_cron = 0;
    }
}

/* This function gets called every time the server is entering the
 * main loop of the event driven library, that is, before to sleep
 * for ready file descriptors.
 *
 * Note: This function is (currently) called from two functions:
 * 1. aeMain - The main server loop
 * 2. processEventsWhileBlocked - Process clients during RDB/AOF load
 *
 * If it was called from processEventsWhileBlocked we don't want
 * to perform all actions (For example, we don't want to expire
 * keys), but we do need to perform some actions.
 *
 * The most important is freeClientsInAsyncFreeQueue but we also
 * call some other low-risk functions. */
void beforeSleep(struct aeEventLoop *eventLoop) {
    UNUSED(eventLoop);

    size_t zmalloc_used = zmalloc_used_memory();
    if (zmalloc_used > server.stat_peak_memory) server.stat_peak_memory = zmalloc_used;

    /* Handle pending data(typical TLS). (must be done before flushAppendOnlyFile) */
    connTypeProcessPendingData();

    /* If any connection type(typical TLS) still has pending unread data don't sleep at all. */
    int dont_sleep = connTypeHasPendingData();

    /* Handle blocked clients.
     * must be done before flushAppendOnlyFile, in case of appendfsync=always,
     * since the unblocked clients may write data. */
    blockedBeforeSleep();

    /* Run a fast expire cycle (the called function will return
     * ASAP if a fast cycle is not needed). */
    if (iAmPrimary()) activeExpireCycle(ACTIVE_EXPIRE_CYCLE_FAST);

    if (moduleCount()) {
        moduleFireServerEvent(VALKEYMODULE_EVENT_EVENTLOOP, VALKEYMODULE_SUBEVENT_EVENTLOOP_BEFORE_SLEEP, NULL);
    }

    /* Since we rely on current_client to send scheduled invalidation messages
     * we have to flush them after each command, so when we get here, the list
     * must be empty. */
    serverAssert(listLength(server.pending_push_messages) == 0);

    /* Handle writes with pending output buffers. */
    handleClientsWithPendingWrites();

    /* Close clients that need to be closed asynchronous */
    freeClientsInAsyncFreeQueue();

    /* Disconnect some clients if they are consuming too much memory. */
    evictClients();

    /* Before we are going to sleep, let the threads access the dataset by
     * releasing the GIL. The server main thread will not touch anything at this
     * time. */
    if (moduleCount()) moduleReleaseGIL();
    /********************* WARNING ********************
     * Do NOT add anything below moduleReleaseGIL !!! *
     ***************************** ********************/
}

/* This function is called immediately after the event loop multiplexing
 * API returned, and the control is going to soon return to the server by invoking
 * the different events callbacks. */
void afterSleep(struct aeEventLoop *eventLoop, int numevents) {
    UNUSED(eventLoop);
    UNUSED(numevents);

    /* Acquire the modules GIL so that their threads won't touch anything. */
    if (moduleCount()) {
        atomic_store_explicit(&server.module_gil_acquiring, 1, memory_order_relaxed);
        moduleAcquireGIL();
        atomic_store_explicit(&server.module_gil_acquiring, 0, memory_order_relaxed);
        moduleFireServerEvent(VALKEYMODULE_EVENT_EVENTLOOP, VALKEYMODULE_SUBEVENT_EVENTLOOP_AFTER_SLEEP, NULL);
    }

    /* Update the time cache. */
    updateCachedTime(1);
}

/* =========================== Server initialization ======================== */

/* These shared strings depend on the extended-redis-compatibility config and is
 * called when the config changes. When the config is phased out, these
 * initializations can be moved back inside createSharedObjects() below. */
void createSharedObjectsWithCompat(void) {
    const char *name = SERVER_TITLE;
    if (shared.loadingerr) decrRefCount(shared.loadingerr);
    shared.loadingerr =
        createObject(OBJ_STRING, sdscatfmt(sdsempty(), "-LOADING %s is loading the dataset in memory\r\n", name));
    if (shared.slowevalerr) decrRefCount(shared.slowevalerr);
    shared.slowevalerr = createObject(
        OBJ_STRING,
        sdscatfmt(sdsempty(),
                  "-BUSY %s is busy running a script. You can only call SCRIPT KILL or SHUTDOWN NOSAVE.\r\n", name));
    if (shared.slowscripterr) decrRefCount(shared.slowscripterr);
    shared.slowscripterr = createObject(
        OBJ_STRING,
        sdscatfmt(sdsempty(),
                  "-BUSY %s is busy running a script. You can only call FUNCTION KILL or SHUTDOWN NOSAVE.\r\n", name));
    if (shared.slowmoduleerr) decrRefCount(shared.slowmoduleerr);
    shared.slowmoduleerr =
        createObject(OBJ_STRING, sdscatfmt(sdsempty(), "-BUSY %s is busy running a module command.\r\n", name));
    if (shared.bgsaveerr) decrRefCount(shared.bgsaveerr);
    shared.bgsaveerr =
        createObject(OBJ_STRING, sdscatfmt(sdsempty(),
                                           "-MISCONF %s is configured to save RDB snapshots, but it's currently"
                                           " unable to persist to disk. Commands that may modify the data set are"
                                           " disabled, because this instance is configured to report errors during"
                                           " writes if RDB snapshotting fails (stop-writes-on-bgsave-error option)."
                                           " Please check the %s logs for details about the RDB error.\r\n",
                                           name, name));
}

void createSharedObjects(void) {
    int j;

    /* Shared command responses */
    shared.ok = createObject(OBJ_STRING, sdsnew("+OK\r\n"));
    shared.emptybulk = createObject(OBJ_STRING, sdsnew("$0\r\n\r\n"));
    shared.czero = createObject(OBJ_STRING, sdsnew(":0\r\n"));
    shared.cone = createObject(OBJ_STRING, sdsnew(":1\r\n"));
    shared.emptyarray = createObject(OBJ_STRING, sdsnew("*0\r\n"));
    shared.pong = createObject(OBJ_STRING, sdsnew("+PONG\r\n"));
    shared.queued = createObject(OBJ_STRING, sdsnew("+QUEUED\r\n"));
    shared.emptyscan = createObject(OBJ_STRING, sdsnew("*2\r\n$1\r\n0\r\n*0\r\n"));
    shared.space = createObject(OBJ_STRING, sdsnew(" "));
    shared.plus = createObject(OBJ_STRING, sdsnew("+"));

    /* Shared command error responses */
    shared.wrongtypeerr =
        createObject(OBJ_STRING, sdsnew("-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"));
    shared.err = createObject(OBJ_STRING, sdsnew("-ERR\r\n"));
    shared.nokeyerr = createObject(OBJ_STRING, sdsnew("-ERR no such key\r\n"));
    shared.syntaxerr = createObject(OBJ_STRING, sdsnew("-ERR syntax error\r\n"));
    shared.sameobjecterr = createObject(OBJ_STRING, sdsnew("-ERR source and destination objects are the same\r\n"));
    shared.outofrangeerr = createObject(OBJ_STRING, sdsnew("-ERR index out of range\r\n"));
    shared.noscripterr = createObject(OBJ_STRING, sdsnew("-NOSCRIPT No matching script.\r\n"));
    createSharedObjectsWithCompat();
    shared.primarydownerr = createObject(
        OBJ_STRING, sdsnew("-MASTERDOWN Link with MASTER is down and replica-serve-stale-data is set to 'no'.\r\n"));
    shared.roreplicaerr =
        createObject(OBJ_STRING, sdsnew("-READONLY You can't write against a read only replica.\r\n"));
    shared.noautherr = createObject(OBJ_STRING, sdsnew("-NOAUTH Authentication required.\r\n"));
    shared.oomerr = createObject(OBJ_STRING, sdsnew("-OOM command not allowed when used memory > 'maxmemory'.\r\n"));
    shared.execaborterr =
        createObject(OBJ_STRING, sdsnew("-EXECABORT Transaction discarded because of previous errors.\r\n"));
    shared.noreplicaserr = createObject(OBJ_STRING, sdsnew("-NOREPLICAS Not enough good replicas to write.\r\n"));
    shared.busykeyerr = createObject(OBJ_STRING, sdsnew("-BUSYKEY Target key name already exists.\r\n"));

    /* The shared NULL depends on the protocol version. */
    shared.null[0] = NULL;
    shared.null[1] = NULL;
    shared.null[2] = createObject(OBJ_STRING, sdsnew("$-1\r\n"));
    shared.null[3] = createObject(OBJ_STRING, sdsnew("_\r\n"));

    shared.nullarray[0] = NULL;
    shared.nullarray[1] = NULL;
    shared.nullarray[2] = createObject(OBJ_STRING, sdsnew("*-1\r\n"));
    shared.nullarray[3] = createObject(OBJ_STRING, sdsnew("_\r\n"));

    shared.emptymap[0] = NULL;
    shared.emptymap[1] = NULL;
    shared.emptymap[2] = createObject(OBJ_STRING, sdsnew("*0\r\n"));
    shared.emptymap[3] = createObject(OBJ_STRING, sdsnew("%0\r\n"));

    shared.emptyset[0] = NULL;
    shared.emptyset[1] = NULL;
    shared.emptyset[2] = createObject(OBJ_STRING, sdsnew("*0\r\n"));
    shared.emptyset[3] = createObject(OBJ_STRING, sdsnew("~0\r\n"));

    for (j = 0; j < PROTO_SHARED_SELECT_CMDS; j++) {
        char dictid_str[64];
        int dictid_len;

        dictid_len = ll2string(dictid_str, sizeof(dictid_str), j);
        shared.select[j] = createObject(
            OBJ_STRING, sdscatprintf(sdsempty(), "*2\r\n$6\r\nSELECT\r\n$%d\r\n%s\r\n", dictid_len, dictid_str));
    }
    shared.messagebulk = createStringObject("$7\r\nmessage\r\n", 13);
    shared.pmessagebulk = createStringObject("$8\r\npmessage\r\n", 14);
    shared.subscribebulk = createStringObject("$9\r\nsubscribe\r\n", 15);
    shared.unsubscribebulk = createStringObject("$11\r\nunsubscribe\r\n", 18);
    shared.ssubscribebulk = createStringObject("$10\r\nssubscribe\r\n", 17);
    shared.sunsubscribebulk = createStringObject("$12\r\nsunsubscribe\r\n", 19);
    shared.smessagebulk = createStringObject("$8\r\nsmessage\r\n", 14);
    shared.psubscribebulk = createStringObject("$10\r\npsubscribe\r\n", 17);
    shared.punsubscribebulk = createStringObject("$12\r\npunsubscribe\r\n", 19);

    /* Shared command names */
    shared.del = createStringObject("DEL", 3);
    shared.unlink = createStringObject("UNLINK", 6);
    shared.rpop = createStringObject("RPOP", 4);
    shared.lpop = createStringObject("LPOP", 4);
    shared.lpush = createStringObject("LPUSH", 5);
    shared.rpoplpush = createStringObject("RPOPLPUSH", 9);
    shared.lmove = createStringObject("LMOVE", 5);
    shared.blmove = createStringObject("BLMOVE", 6);
    shared.zpopmin = createStringObject("ZPOPMIN", 7);
    shared.zpopmax = createStringObject("ZPOPMAX", 7);
    shared.multi = createStringObject("MULTI", 5);
    shared.exec = createStringObject("EXEC", 4);
    shared.hset = createStringObject("HSET", 4);
    shared.srem = createStringObject("SREM", 4);
    shared.xgroup = createStringObject("XGROUP", 6);
    shared.xclaim = createStringObject("XCLAIM", 6);
    shared.script = createStringObject("SCRIPT", 6);
    shared.replconf = createStringObject("REPLCONF", 8);
    shared.pexpireat = createStringObject("PEXPIREAT", 9);
    shared.pexpire = createStringObject("PEXPIRE", 7);
    shared.persist = createStringObject("PERSIST", 7);
    shared.set = createStringObject("SET", 3);
    shared.eval = createStringObject("EVAL", 4);

    /* Shared command argument */
    shared.left = createStringObject("left", 4);
    shared.right = createStringObject("right", 5);
    shared.pxat = createStringObject("PXAT", 4);
    shared.time = createStringObject("TIME", 4);
    shared.retrycount = createStringObject("RETRYCOUNT", 10);
    shared.force = createStringObject("FORCE", 5);
    shared.justid = createStringObject("JUSTID", 6);
    shared.entriesread = createStringObject("ENTRIESREAD", 11);
    shared.lastid = createStringObject("LASTID", 6);
    shared.default_username = createStringObject("default", 7);
    shared.ping = createStringObject("ping", 4);
    shared.setid = createStringObject("SETID", 5);
    shared.keepttl = createStringObject("KEEPTTL", 7);
    shared.absttl = createStringObject("ABSTTL", 6);
    shared.load = createStringObject("LOAD", 4);
    shared.createconsumer = createStringObject("CREATECONSUMER", 14);
    shared.getack = createStringObject("GETACK", 6);
    shared.special_asterick = createStringObject("*", 1);
    shared.special_equals = createStringObject("=", 1);
    shared.redacted = makeObjectShared(createStringObject("(redacted)", 10));

    for (j = 0; j < OBJ_SHARED_INTEGERS; j++) {
        shared.integers[j] = makeObjectShared(createObject(OBJ_STRING, (void *)(long)j));
        initObjectLRUOrLFU(shared.integers[j]);
        shared.integers[j]->encoding = OBJ_ENCODING_INT;
    }
    for (j = 0; j < OBJ_SHARED_BULKHDR_LEN; j++) {
        shared.mbulkhdr[j] = createObject(OBJ_STRING, sdscatprintf(sdsempty(), "*%d\r\n", j));
        shared.bulkhdr[j] = createObject(OBJ_STRING, sdscatprintf(sdsempty(), "$%d\r\n", j));
        shared.maphdr[j] = createObject(OBJ_STRING, sdscatprintf(sdsempty(), "%%%d\r\n", j));
        shared.sethdr[j] = createObject(OBJ_STRING, sdscatprintf(sdsempty(), "~%d\r\n", j));
    }
    /* The following two shared objects, minstring and maxstring, are not
     * actually used for their value but as a special object meaning
     * respectively the minimum possible string and the maximum possible
     * string in string comparisons for the ZRANGEBYLEX command. */
    shared.minstring = sdsnew("minstring");
    shared.maxstring = sdsnew("maxstring");
}

void initServerClientMemUsageBuckets(void) {
    if (server.client_mem_usage_buckets) return;
    server.client_mem_usage_buckets = zmalloc(sizeof(clientMemUsageBucket) * CLIENT_MEM_USAGE_BUCKETS);
    for (int j = 0; j < CLIENT_MEM_USAGE_BUCKETS; j++) {
        server.client_mem_usage_buckets[j].mem_usage_sum = 0;
        server.client_mem_usage_buckets[j].clients = listCreate();
    }
}

void freeServerClientMemUsageBuckets(void) {
    if (!server.client_mem_usage_buckets) return;
    for (int j = 0; j < CLIENT_MEM_USAGE_BUCKETS; j++) listRelease(server.client_mem_usage_buckets[j].clients);
    zfree(server.client_mem_usage_buckets);
    server.client_mem_usage_buckets = NULL;
}

void initServerConfig(void) {
    int j;
    initConfigValues();
    updateCachedTime(1);
    server.cmd_time_snapshot = server.mstime;
    server.hz = CONFIG_DEFAULT_HZ;   /* Initialize it ASAP, even if it may get
                                        updated later after loading the config.
                                        This value may be used before the server
                                        is initialized. */
    server.timezone = getTimeZone(); /* Initialized by tzset(). */
    server.configfile = NULL;
    server.executable = NULL;
    server.arch_bits = (sizeof(long) == 8) ? 64 : 32;
    server.lazy_expire_disabled = 0;
    server.loading = 0;
    server.async_loading = 0;
    server.notify_keyspace_events = 0;
    server.blocked_clients = 0;
    memset(server.blocked_clients_by_type, 0, sizeof(server.blocked_clients_by_type));
    server.next_client_id = 1; /* Client IDs, start from 1 .*/
    server.page_size = sysconf(_SC_PAGESIZE);

    server.lruclock = getLRUClock();
    /* Client output buffer limits */
    for (j = 0; j < CLIENT_TYPE_OBUF_COUNT; j++) server.client_obuf_limits[j] = clientBufferLimitsDefaults[j];

    /* Linux OOM Score config */
    for (j = 0; j < CONFIG_OOM_COUNT; j++) server.oom_score_adj_values[j] = configOOMScoreAdjValuesDefaults[j];

    /* Double constants initialization */
    R_Zero = 0.0;
    R_PosInf = 1.0 / R_Zero;
    R_NegInf = -1.0 / R_Zero;
    R_Nan = R_Zero / R_Zero;

    /* Command table -- we initialize it here as it is part of the
     * initial configuration, since command names may be changed via
     * valkey.conf using the rename-command directive. */
    server.commands = dictCreate(&commandTableDictType);
    server.orig_commands = dictCreate(&commandTableDictType);
    populateCommandTable();
}

extern char **environ;

/* This function will configure the current process's oom_score_adj according
 * to user specified configuration. This is currently implemented on Linux
 * only.
 *
 * A process_class value of -1 implies OOM_CONFIG_PRIMARY or OOM_CONFIG_REPLICA,
 * depending on current role.
 */
int setOOMScoreAdj(int process_class) {
#ifdef HAVE_PROC_OOM_SCORE_ADJ
    /* The following statics are used to indicate the server has changed the process's oom score.
     * And to save the original score so we can restore it later if needed.
     * We need this so when we disabled oom-score-adj (also during configuration rollback
     * when another configuration parameter was invalid and causes a rollback after
     * applying a new oom-score) we can return to the oom-score value from before our
     * adjustments. */
    static int oom_score_adjusted_by_valkey = 0;
    static int oom_score_adj_base = 0;

    int fd;
    int val;
    char buf[64];

    if (server.oom_score_adj != OOM_SCORE_ADJ_NO) {
        if (!oom_score_adjusted_by_valkey) {
            oom_score_adjusted_by_valkey = 1;
            /* Backup base value before enabling the server control over oom score */
            fd = open("/proc/self/oom_score_adj", O_RDONLY);
            if (fd < 0 || read(fd, buf, sizeof(buf)) < 0) {
                serverLog(LL_WARNING, "Unable to read oom_score_adj: %s", strerror(errno));
                if (fd != -1) close(fd);
                return C_ERR;
            }
            oom_score_adj_base = atoi(buf);
            close(fd);
        }

        val = server.oom_score_adj_values[process_class];
        if (server.oom_score_adj == OOM_SCORE_RELATIVE) val += oom_score_adj_base;
        if (val > 1000) val = 1000;
        if (val < -1000) val = -1000;
    } else if (oom_score_adjusted_by_valkey) {
        oom_score_adjusted_by_valkey = 0;
        val = oom_score_adj_base;
    } else {
        return C_OK;
    }

    snprintf(buf, sizeof(buf) - 1, "%d\n", val);

    fd = open("/proc/self/oom_score_adj", O_WRONLY);
    if (fd < 0 || write(fd, buf, strlen(buf)) < 0) {
        serverLog(LL_WARNING, "Unable to write oom_score_adj: %s", strerror(errno));
        if (fd != -1) close(fd);
        return C_ERR;
    }

    close(fd);
    return C_OK;
#else
    /* Unsupported */
    return C_ERR;
#endif
}

/* Resets the stats that we expose via INFO or other means that we want
 * to reset via CONFIG RESETSTAT. The function is also used in order to
 * initialize these fields in initServer() at server startup. */
void resetServerStats(void) {
    server.stat_numcommands = 0;
    server.stat_numconnections = 0;
    server.stat_expiredkeys = 0;
    server.stat_expired_stale_perc = 0;
    server.stat_expired_time_cap_reached_count = 0;
    server.stat_expire_cycle_time_used = 0;
    server.stat_evictedkeys = 0;
    server.stat_evictedclients = 0;
    server.stat_evictedscripts = 0;
    server.stat_total_eviction_exceeded_time = 0;
    server.stat_last_eviction_exceeded_time = 0;
    server.stat_keyspace_misses = 0;
    server.stat_keyspace_hits = 0;
    server.stat_active_defrag_hits = 0;
    server.stat_active_defrag_misses = 0;
    server.stat_active_defrag_key_hits = 0;
    server.stat_active_defrag_key_misses = 0;
    server.stat_active_defrag_scanned = 0;
    server.stat_total_active_defrag_time = 0;
    server.stat_last_active_defrag_time = 0;
    server.stat_fork_time = 0;
    server.stat_fork_rate = 0;
    server.stat_total_forks = 0;
    server.stat_rejected_conn = 0;
    server.stat_sync_full = 0;
    server.stat_sync_partial_ok = 0;
    server.stat_sync_partial_err = 0;
    server.stat_total_reads_processed = 0;
    server.stat_client_qbuf_limit_disconnections = 0;
    server.stat_client_outbuf_limit_disconnections = 0;
    server.stat_aof_rewrites = 0;
    server.stat_rdb_saves = 0;
    server.stat_aofrw_consecutive_failures = 0;
    server.stat_net_input_bytes = 0;
    server.stat_net_output_bytes = 0;
    server.stat_net_repl_input_bytes = 0;
    server.stat_net_repl_output_bytes = 0;
    server.stat_unexpected_error_replies = 0;
    server.stat_total_error_replies = 0;
    server.stat_dump_payload_sanitizations = 0;
    server.stat_reply_buffer_shrinks = 0;
    server.stat_reply_buffer_expands = 0;
    lazyfreeResetStats();
}

void initServer(void) {
    int j;

    /* Initialization after setting defaults from the config system. */
    server.hz = server.config_hz;
    server.current_client = NULL;
    server.errors = raxNew();
    server.execution_nesting = 0;
    server.clients = listCreate();
    server.clients_index = raxNew();
    server.clients_to_close = listCreate();
    server.replicas = listCreate();
    server.monitors = listCreate();
    server.clients_pending_write = listCreate();
    server.clients_pending_io_write = listCreate();
    server.clients_pending_io_read = listCreate();
    server.clients_timeout_table = raxNew();
    server.replication_allowed = 1;
    server.unblocked_clients = listCreate();
    server.ready_keys = listCreate();
    server.pending_push_messages = listCreate();
    server.clients_waiting_acks = listCreate();
    server.paused_actions = 0;
    memset(server.client_pause_per_purpose, 0, sizeof(server.client_pause_per_purpose));
    server.postponed_clients = listCreate();
    server.blocked_last_cron = 0;
    server.blocking_op_nesting = 0;
    server.reply_buffer_peak_reset_time = REPLY_BUFFER_DEFAULT_PEAK_RESET_TIME;
    server.client_mem_usage_buckets = NULL;
    server.debug_client_enforce_reply_list = 0;

    createSharedObjects();
    const char *clk_msg = monotonicInit();
    serverLog(LL_NOTICE, "monotonic clock: %s", clk_msg);
    server.db = zmalloc(sizeof(serverDb) * server.dbnum);

    /* Create the databases, and initialize other internal state. */
    int slot_count_bits = 0;
    int flags = KVSTORE_ALLOCATE_DICTS_ON_DEMAND;
    for (j = 0; j < server.dbnum; j++) {
        server.db[j].keys = kvstoreCreate(&kvstoreKeysDictType, slot_count_bits, flags);
        server.db[j].expires = kvstoreCreate(&kvstoreExpiresDictType, slot_count_bits, flags);
        server.db[j].expires_cursor = 0;
        server.db[j].blocking_keys = dictCreate(&keylistDictType);
        server.db[j].blocking_keys_unblock_on_nokey = dictCreate(&objectKeyPointerValueDictType);
        server.db[j].ready_keys = dictCreate(&objectKeyPointerValueDictType);
        server.db[j].watched_keys = dictCreate(&keylistDictType);
        server.db[j].id = j;
        server.db[j].avg_ttl = 0;
        server.db[j].defrag_later = listCreate();
        listSetFreeMethod(server.db[j].defrag_later, (void (*)(void *))sdsfree);
    }
    evictionPoolAlloc(); /* Initialize the LRU keys pool. */
    /* Note that server.pubsub_channels was chosen to be a kvstore (with only one dict, which
     * seems odd) just to make the code cleaner by making it be the same type as server.pubsubshard_channels
     * (which has to be kvstore), see pubsubtype.serverPubSubChannels */
    server.pubsub_channels = kvstoreCreate(&kvstoreChannelDictType, 0, KVSTORE_ALLOCATE_DICTS_ON_DEMAND);
    server.pubsub_patterns = dictCreate(&objToDictDictType);
    server.pubsubshard_channels = kvstoreCreate(&kvstoreChannelDictType, slot_count_bits,
                                                KVSTORE_ALLOCATE_DICTS_ON_DEMAND | KVSTORE_FREE_EMPTY_DICTS);
    server.pubsub_clients = 0;
    server.watching_clients = 0;
    server.cronloops = 0;
    server.in_exec = 0;
    server.busy_module_yield_flags = BUSY_MODULE_YIELD_NONE;
    server.busy_module_yield_reply = NULL;
    server.client_pause_in_transaction = 0;
    server.dirty = 0;
    server.crashed = 0;
    resetServerStats();
    /* A few stats we don't want to reset: server startup time, and peak mem. */
    server.stat_starttime = time(NULL);
    server.stat_peak_memory = 0;
    server.stat_current_cow_peak = 0;
    server.stat_current_cow_bytes = 0;
    server.stat_current_cow_updated = 0;
    server.stat_current_save_keys_processed = 0;
    server.stat_current_save_keys_total = 0;
    server.stat_rdb_cow_bytes = 0;
    server.stat_aof_cow_bytes = 0;
    server.stat_module_cow_bytes = 0;
    server.stat_module_progress = 0;
    for (int j = 0; j < CLIENT_TYPE_COUNT; j++) server.stat_clients_type_memory[j] = 0;
    server.stat_cluster_links_memory = 0;
    server.cron_malloc_stats.zmalloc_used = 0;
    server.cron_malloc_stats.process_rss = 0;
    server.cron_malloc_stats.allocator_allocated = 0;
    server.cron_malloc_stats.allocator_active = 0;
    server.cron_malloc_stats.allocator_resident = 0;
    server.last_sig_received = 0;

    /* 32 bit instances are limited to 4GB of address space, so if there is
     * no explicit limit in the user provided configuration we set a limit
     * at 3 GB using maxmemory with 'noeviction' policy'. This avoids
     * useless crashes of the instance for out of memory. */
    if (server.arch_bits == 32 && server.maxmemory == 0) {
        serverLog(LL_WARNING, "Warning: 32 bit instance detected but no memory limit set. Setting 3 GB maxmemory limit "
                              "with 'noeviction' policy now.");
        server.maxmemory = 3072LL * (1024 * 1024); /* 3 GB */
        server.maxmemory_policy = MAXMEMORY_NO_EVICTION;
    }

    scriptingInit(1);
    if (functionsInit() == C_ERR) {
        serverPanic("Functions initialization failed, check the server logs.");
        exit(1);
    }
    initSharedQueryBuf();

    if (server.maxmemory_clients != 0) initServerClientMemUsageBuckets();
}

/* Some steps in server initialization need to be done last (after modules
 * are loaded).
 * Specifically, creation of threads due to a race bug in ld.so, in which
 * Thread Local Storage initialization collides with dlopen call.
 * see: https://sourceware.org/bugzilla/show_bug.cgi?id=19329 */
void InitServerLast(void) {
    bioInit();
    set_jemalloc_bg_thread(server.jemalloc_bg_thread);
    server.initial_memory_usage = zmalloc_used_memory();
}

/* The purpose of this function is to try to "glue" consecutive range
 * key specs in order to build the legacy (first,last,step) spec
 * used by the COMMAND command.
 * By far the most common case is just one range spec (e.g. SET)
 * but some commands' ranges were split into two or more ranges
 * in order to have different flags for different keys (e.g. SMOVE,
 * first key is "RW ACCESS DELETE", second key is "RW INSERT").
 *
 * Additionally set the CMD_MOVABLE_KEYS flag for commands that may have key
 * names in their arguments, but the legacy range spec doesn't cover all of them.
 *
 * This function uses very basic heuristics and is "best effort":
 * 1. Only commands which have only "range" specs are considered.
 * 2. Only range specs with keystep of 1 are considered.
 * 3. The order of the range specs must be ascending (i.e.
 *    lastkey of spec[i] == firstkey-1 of spec[i+1]).
 *
 * This function will succeed on all native commands and may
 * fail on module commands, even if it only has "range" specs that
 * could actually be "glued", in the following cases:
 * 1. The order of "range" specs is not ascending (e.g. the spec for
 *    the key at index 2 was added before the spec of the key at
 *    index 1).
 * 2. The "range" specs have keystep >1.
 *
 * If this functions fails it means that the legacy (first,last,step)
 * spec used by COMMAND will show 0,0,0. This is not a dire situation
 * because anyway the legacy (first,last,step) spec is to be deprecated
 * and one should use the new key specs scheme.
 */
void populateCommandLegacyRangeSpec(struct serverCommand *c) {
    memset(&c->legacy_range_key_spec, 0, sizeof(c->legacy_range_key_spec));

    /* Set the movablekeys flag if we have a GETKEYS flag for modules.
     * Note that for native commands, we always have keyspecs,
     * with enough information to rely on for movablekeys. */
    if (c->flags & CMD_MODULE_GETKEYS) c->flags |= CMD_MOVABLE_KEYS;

    /* no key-specs, no keys, exit. */
    if (c->key_specs_num == 0) {
        return;
    }

    if (c->key_specs_num == 1 && c->key_specs[0].begin_search_type == KSPEC_BS_INDEX &&
        c->key_specs[0].find_keys_type == KSPEC_FK_RANGE) {
        /* Quick win, exactly one range spec. */
        c->legacy_range_key_spec = c->key_specs[0];
        /* If it has the incomplete flag, set the movablekeys flag on the command. */
        if (c->key_specs[0].flags & CMD_KEY_INCOMPLETE) c->flags |= CMD_MOVABLE_KEYS;
        return;
    }

    int firstkey = INT_MAX, lastkey = 0;
    int prev_lastkey = 0;
    for (int i = 0; i < c->key_specs_num; i++) {
        if (c->key_specs[i].begin_search_type != KSPEC_BS_INDEX || c->key_specs[i].find_keys_type != KSPEC_FK_RANGE) {
            /* Found an incompatible (non range) spec, skip it, and set the movablekeys flag. */
            c->flags |= CMD_MOVABLE_KEYS;
            continue;
        }
        if (c->key_specs[i].fk.range.keystep != 1 ||
            (prev_lastkey && prev_lastkey != c->key_specs[i].bs.index.pos - 1)) {
            /* Found a range spec that's not plain (step of 1) or not consecutive to the previous one.
             * Skip it, and we set the movablekeys flag. */
            c->flags |= CMD_MOVABLE_KEYS;
            continue;
        }
        if (c->key_specs[i].flags & CMD_KEY_INCOMPLETE) {
            /* The spec we're using is incomplete, we can use it, but we also have to set the movablekeys flag. */
            c->flags |= CMD_MOVABLE_KEYS;
        }
        firstkey = min(firstkey, c->key_specs[i].bs.index.pos);
        /* Get the absolute index for lastkey (in the "range" spec, lastkey is relative to firstkey) */
        int lastkey_abs_index = c->key_specs[i].fk.range.lastkey;
        if (lastkey_abs_index >= 0) lastkey_abs_index += c->key_specs[i].bs.index.pos;
        /* For lastkey we use unsigned comparison to handle negative values correctly */
        lastkey = max((unsigned)lastkey, (unsigned)lastkey_abs_index);
        prev_lastkey = lastkey;
    }

    if (firstkey == INT_MAX) {
        /* Couldn't find range specs, the legacy range spec will remain empty, and we set the movablekeys flag. */
        c->flags |= CMD_MOVABLE_KEYS;
        return;
    }

    serverAssert(firstkey != 0);
    serverAssert(lastkey != 0);

    c->legacy_range_key_spec.begin_search_type = KSPEC_BS_INDEX;
    c->legacy_range_key_spec.bs.index.pos = firstkey;
    c->legacy_range_key_spec.find_keys_type = KSPEC_FK_RANGE;
    c->legacy_range_key_spec.fk.range.lastkey =
        lastkey < 0 ? lastkey : (lastkey - firstkey); /* in the "range" spec, lastkey is relative to firstkey */
    c->legacy_range_key_spec.fk.range.keystep = 1;
    c->legacy_range_key_spec.fk.range.limit = 0;
}

sds catSubCommandFullname(const char *parent_name, const char *sub_name) {
    return sdscatfmt(sdsempty(), "%s|%s", parent_name, sub_name);
}

void commandAddSubcommand(struct serverCommand *parent, struct serverCommand *subcommand, const char *declared_name) {
    if (!parent->subcommands_dict) parent->subcommands_dict = dictCreate(&commandTableDictType);

    subcommand->parent = parent;                            /* Assign the parent command */
    subcommand->id = 0; /* Assign the ID used for ACL. */

    serverAssert(dictAdd(parent->subcommands_dict, sdsnew(declared_name), subcommand) == DICT_OK);
}

/* Set implicit ACl categories (see comment above the definition of
 * struct serverCommand). */
void setImplicitACLCategories(struct serverCommand *c) {
    if (c->flags & CMD_WRITE) c->acl_categories |= ACL_CATEGORY_WRITE;
    /* Exclude scripting commands from the RO category. */
    if (c->flags & CMD_READONLY && !(c->acl_categories & ACL_CATEGORY_SCRIPTING))
        c->acl_categories |= ACL_CATEGORY_READ;
    if (c->flags & CMD_ADMIN) c->acl_categories |= ACL_CATEGORY_ADMIN | ACL_CATEGORY_DANGEROUS;
    if (c->flags & CMD_PUBSUB) c->acl_categories |= ACL_CATEGORY_PUBSUB;
    if (c->flags & CMD_FAST) c->acl_categories |= ACL_CATEGORY_FAST;
    if (c->flags & CMD_BLOCKING) c->acl_categories |= ACL_CATEGORY_BLOCKING;

    /* If it's not @fast is @slow in this binary world. */
    if (!(c->acl_categories & ACL_CATEGORY_FAST)) c->acl_categories |= ACL_CATEGORY_SLOW;
}

/* Recursively populate the command structure.
 *
 * On success, the function return C_OK. Otherwise C_ERR is returned and we won't
 * add this command in the commands dict. */
int populateCommandStructure(struct serverCommand *c) {
    /* Translate the command string flags description into an actual
     * set of flags. */
    setImplicitACLCategories(c);

    /* We start with an unallocated histogram and only allocate memory when a command
     * has been issued for the first time */
    c->latency_histogram = NULL;

    /* Handle the legacy range spec and the "movablekeys" flag (must be done after populating all key specs). */
    populateCommandLegacyRangeSpec(c);

    /* Assign the ID used for ACL. */
    c->id = 0;

    /* Handle subcommands */
    if (c->subcommands) {
        for (int j = 0; c->subcommands[j].declared_name; j++) {
            struct serverCommand *sub = c->subcommands + j;

            sub->fullname = catSubCommandFullname(c->declared_name, sub->declared_name);
            if (populateCommandStructure(sub) == C_ERR) continue;

            commandAddSubcommand(c, sub, sub->declared_name);
        }
    }

    return C_OK;
}

extern struct serverCommand serverCommandTable[];

/* Populates the Command Table dict from the static table in commands.c
 * which is auto generated from the json files in the commands folder. */
void populateCommandTable(void) {
    int j;
    struct serverCommand *c;

    for (j = 0;; j++) {
        c = serverCommandTable + j;
        if (c->declared_name == NULL) break;

        int retval1, retval2;

        c->fullname = sdsnew(c->declared_name);
        if (populateCommandStructure(c) == C_ERR) continue;

        retval1 = dictAdd(server.commands, sdsdup(c->fullname), c);
        /* Populate an additional dictionary that will be unaffected
         * by rename-command statements in valkey.conf. */
        retval2 = dictAdd(server.orig_commands, sdsdup(c->fullname), c);
        serverAssert(retval1 == DICT_OK && retval2 == DICT_OK);
    }
}

void resetCommandTableStats(dict *commands) {
    struct serverCommand *c;
    dictEntry *de;
    dictIterator *di;

    di = dictGetSafeIterator(commands);
    while ((de = dictNext(di)) != NULL) {
        c = (struct serverCommand *)dictGetVal(de);
        c->microseconds = 0;
        c->calls = 0;
        c->rejected_calls = 0;
        c->failed_calls = 0;
        if (c->latency_histogram) {
            hdr_close(c->latency_histogram);
            c->latency_histogram = NULL;
        }
        if (c->subcommands_dict) resetCommandTableStats(c->subcommands_dict);
    }
    dictReleaseIterator(di);
}

void resetErrorTableStats(void) {
    freeErrorsRadixTreeAsync(server.errors);
    server.errors = raxNew();
}

/* ========================== OP Array API ============================ */

int serverOpArrayAppend(serverOpArray *oa, int dbid, robj **argv, int argc, int target) {
    serverOp *op;
    int prev_capacity = oa->capacity;

    if (oa->numops == 0) {
        oa->capacity = 16;
    } else if (oa->numops >= oa->capacity) {
        oa->capacity *= 2;
    }

    if (prev_capacity != oa->capacity) oa->ops = zrealloc(oa->ops, sizeof(serverOp) * oa->capacity);
    op = oa->ops + oa->numops;
    op->dbid = dbid;
    op->argv = argv;
    op->argc = argc;
    op->target = target;
    oa->numops++;
    return oa->numops;
}

void serverOpArrayFree(serverOpArray *oa) {
    while (oa->numops) {
        int j;
        serverOp *op;

        oa->numops--;
        op = oa->ops + oa->numops;
        for (j = 0; j < op->argc; j++) decrRefCount(op->argv[j]);
        zfree(op->argv);
    }
    /* no need to free the actual op array, we reuse the memory for future commands */
    serverAssert(!oa->numops);
}

/* ====================== Commands lookup and execution ===================== */

int isContainerCommandBySds(sds s) {
    struct serverCommand *base_cmd = dictFetchValue(server.commands, s);
    int has_subcommands = base_cmd && base_cmd->subcommands_dict;
    return has_subcommands;
}

struct serverCommand *lookupSubcommand(struct serverCommand *container, sds sub_name) {
    return dictFetchValue(container->subcommands_dict, sub_name);
}

/* Look up a command by argv and argc
 *
 * If `strict` is not 0 we expect argc to be exact (i.e. argc==2
 * for a subcommand and argc==1 for a top-level command)
 * `strict` should be used every time we want to look up a command
 * name (e.g. in COMMAND INFO) rather than to find the command
 * a user requested to execute (in processCommand).
 */
struct serverCommand *lookupCommandLogic(dict *commands, robj **argv, int argc, int strict) {
    struct serverCommand *base_cmd = dictFetchValue(commands, argv[0]->ptr);
    int has_subcommands = base_cmd && base_cmd->subcommands_dict;
    if (argc == 1 || !has_subcommands) {
        if (strict && argc != 1) return NULL;
        /* Note: It is possible that base_cmd->proc==NULL (e.g. CONFIG) */
        return base_cmd;
    } else { /* argc > 1 && has_subcommands */
        if (strict && argc != 2) return NULL;
        /* Note: Currently we support just one level of subcommands */
        return lookupSubcommand(base_cmd, argv[1]->ptr);
    }
}

struct serverCommand *lookupCommand(robj **argv, int argc) {
    return lookupCommandLogic(server.commands, argv, argc, 0);
}

struct serverCommand *lookupCommandBySdsLogic(dict *commands, sds s) {
    int argc, j;
    sds *strings = sdssplitlen(s, sdslen(s), "|", 1, &argc);
    if (strings == NULL) return NULL;
    if (argc < 1 || argc > 2) {
        /* Currently we support just one level of subcommands */
        sdsfreesplitres(strings, argc);
        return NULL;
    }

    serverAssert(argc > 0); /* Avoid warning `-Wmaybe-uninitialized` in lookupCommandLogic() */
    robj objects[argc];
    robj *argv[argc];
    for (j = 0; j < argc; j++) {
        initStaticStringObject(objects[j], strings[j]);
        argv[j] = &objects[j];
    }

    struct serverCommand *cmd = lookupCommandLogic(commands, argv, argc, 1);
    sdsfreesplitres(strings, argc);
    return cmd;
}

struct serverCommand *lookupCommandBySds(sds s) {
    return lookupCommandBySdsLogic(server.commands, s);
}

struct serverCommand *lookupCommandByCStringLogic(dict *commands, const char *s) {
    struct serverCommand *cmd;
    sds name = sdsnew(s);

    cmd = lookupCommandBySdsLogic(commands, name);
    sdsfree(name);
    return cmd;
}

struct serverCommand *lookupCommandByCString(const char *s) {
    return lookupCommandByCStringLogic(server.commands, s);
}

/* Lookup the command in the current table, if not found also check in
 * the original table containing the original command names unaffected by
 * valkey.conf rename-command statement.
 *
 * This is used by functions rewriting the argument vector such as
 * rewriteClientCommandVector() in order to set client->cmd pointer
 * correctly even if the command was renamed. */
struct serverCommand *lookupCommandOrOriginal(robj **argv, int argc) {
    struct serverCommand *cmd = lookupCommandLogic(server.commands, argv, argc, 0);

    if (!cmd) cmd = lookupCommandLogic(server.orig_commands, argv, argc, 0);
    return cmd;
}

/* Commands arriving from the primary client or AOF client, should never be rejected. */
int mustObeyClient(client *c) {
    return c->id == CLIENT_ID_AOF || c->flag.primary;
}

/* Propagate the specified command (in the context of the specified database id)
 * to AOF and replicas.
 *
 * flags are an xor between:
 * + PROPAGATE_NONE (no propagation of command at all)
 * + PROPAGATE_AOF (propagate into the AOF file if is enabled)
 * + PROPAGATE_REPL (propagate into the replication link)
 *
 * This is an internal low-level function and should not be called!
 *
 * The API for propagating commands is alsoPropagate().
 *
 * dbid value of -1 is saved to indicate that the called do not want
 * to replicate SELECT for this command (used for database neutral commands).
 */
static void propagateNow(int dbid, robj **argv, int argc, int target) {
    UNUSED(dbid);
    UNUSED(argv);
    UNUSED(argc);
    UNUSED(target);

    /* This needs to be unreachable since the dataset should be fixed during
     * replica pause (otherwise data may be lost during a failover).
     *
     * Though, there are exceptions:
     *
     * 1. We allow write commands that were queued up before and after to
     *    execute, if a CLIENT PAUSE executed during a transaction, we will
     *    track the state, the CLIENT PAUSE takes effect only after a transaction
     *    has finished.
     * 2. Primary loses a slot during the pause, deletes all keys and replicates
     *    DEL to its replicas. In this case, we will track the state, the dirty
     *    slots will be deleted in the end without affecting the data consistency.
     *
     * Note that case 2 can happen in one of the following scenarios:
     * 1) The primary waits for the replica to replicate before exiting, see
     *    shutdown-timeout in conf for more details. In this case, primary lost
     *    a slot during the SIGTERM waiting.
     * 2) The primary waits for the replica to replicate during a manual failover.
     *    In this case, primary lost a slot during the pausing.
     * 3) The primary was paused by CLIENT PAUSE, and lost a slot during the
     *    pausing. */
    serverAssert(!isPausedActions(PAUSE_ACTION_REPLICA) || server.client_pause_in_transaction);

    // if (target & PROPAGATE_REPL) replicationFeedReplicas(dbid, argv, argc);
}

/* Used inside commands to schedule the propagation of additional commands
 * after the current command is propagated to AOF / Replication.
 *
 * dbid is the database ID the command should be propagated into.
 * Arguments of the command to propagate are passed as an array of
 * Objects pointers of len 'argc', using the 'argv' vector.
 *
 * The function does not take a reference to the passed 'argv' vector,
 * so it is up to the caller to release the passed argv (but it is usually
 * stack allocated).  The function automatically increments ref count of
 * passed objects, so the caller does not need to. */
void alsoPropagate(int dbid, robj **argv, int argc, int target) {
    robj **argvcopy;
    int j;

    argvcopy = zmalloc(sizeof(robj *) * argc);
    for (j = 0; j < argc; j++) {
        argvcopy[j] = argv[j];
        incrRefCount(argv[j]);
    }
    serverOpArrayAppend(&server.also_propagate, dbid, argvcopy, argc, target);
}

/* It is possible to call the function forceCommandPropagation() inside a
 * command implementation in order to to force the propagation of a
 * specific command execution into AOF / Replication. */
void forceCommandPropagation(client *c, int flags) {
    serverAssert(c->cmd->flags & (CMD_WRITE | CMD_MAY_REPLICATE));
    if (flags & PROPAGATE_REPL) c->flag.force_repl = 1;
    if (flags & PROPAGATE_AOF) c->flag.force_aof = 1;
}

/* Avoid that the executed command is propagated at all. This way we
 * are free to just propagate what we want using the alsoPropagate()
 * API. */
void preventCommandPropagation(client *c) {
    c->flag.prevent_prop = 1;
}

/* AOF specific version of preventCommandPropagation(). */
void preventCommandAOF(client *c) {
    c->flag.prevent_aof_prop = 1;
}

/* Replication specific version of preventCommandPropagation(). */
void preventCommandReplication(client *c) {
    c->flag.prevent_repl_prop = 1;
}

/* This function is called in order to update the total command histogram duration.
 * The latency unit is nano-seconds.
 * If needed it will allocate the histogram memory and trim the duration to the upper/lower tracking limits*/
void updateCommandLatencyHistogram(struct hdr_histogram **latency_histogram, int64_t duration_hist) {
    if (duration_hist < LATENCY_HISTOGRAM_MIN_VALUE) duration_hist = LATENCY_HISTOGRAM_MIN_VALUE;
    if (duration_hist > LATENCY_HISTOGRAM_MAX_VALUE) duration_hist = LATENCY_HISTOGRAM_MAX_VALUE;
    if (*latency_histogram == NULL)
        hdr_init(LATENCY_HISTOGRAM_MIN_VALUE, LATENCY_HISTOGRAM_MAX_VALUE, LATENCY_HISTOGRAM_PRECISION,
                 latency_histogram);
    hdr_record_value(*latency_histogram, duration_hist);
}

/* Handle the alsoPropagate() API to handle commands that want to propagate
 * multiple separated commands. Note that alsoPropagate() is not affected
 * by CLIENT_PREVENT_PROP flag. */
static void propagatePendingCommands(void) {
    if (server.also_propagate.numops == 0) return;

    int j;
    serverOp *rop;

    /* If we got here it means we have finished an execution-unit.
     * If that unit has caused propagation of multiple commands, they
     * should be propagated as a transaction */
    int transaction = server.also_propagate.numops > 1;

    /* In case a command that may modify random keys was run *directly*
     * (i.e. not from within a script, MULTI/EXEC, RM_Call, etc.) we want
     * to avoid using a transaction (much like active-expire) */
    if (server.current_client && server.current_client->cmd &&
        server.current_client->cmd->flags & CMD_TOUCHES_ARBITRARY_KEYS) {
        transaction = 0;
    }

    if (transaction) {
        /* We use dbid=-1 to indicate we do not want to replicate SELECT.
         * It'll be inserted together with the next command (inside the MULTI) */
        propagateNow(-1, &shared.multi, 1, PROPAGATE_AOF | PROPAGATE_REPL);
    }

    for (j = 0; j < server.also_propagate.numops; j++) {
        rop = &server.also_propagate.ops[j];
        serverAssert(rop->target);
        propagateNow(rop->dbid, rop->argv, rop->argc, rop->target);
    }

    if (transaction) {
        /* We use dbid=-1 to indicate we do not want to replicate select */
        propagateNow(-1, &shared.exec, 1, PROPAGATE_AOF | PROPAGATE_REPL);
    }

    serverOpArrayFree(&server.also_propagate);
}

/* Performs operations that should be performed after an execution unit ends.
 * Execution unit is a code that should be done atomically.
 * Execution units can be nested and do not necessarily start with a server command.
 *
 * For example the following is a logical unit:
 *   active expire ->
 *      trigger del notification of some module ->
 *          accessing a key ->
 *              trigger key miss notification of some other module
 *
 * What we want to achieve is that the entire execution unit will be done atomically,
 * currently with respect to replication and post jobs, but in the future there might
 * be other considerations. So we basically want the `postUnitOperations` to trigger
 * after the entire chain finished. */
void postExecutionUnitOperations(void) {
    if (server.execution_nesting) return;

    firePostExecutionUnitJobs();

    /* If we are at the top-most call() and not inside a an active module
     * context (e.g. within a module timer) we can propagate what we accumulated. */
    propagatePendingCommands();

    /* Module subsystem post-execution-unit logic */
    modulePostExecutionUnitOperations();
}

/* Increment the command failure counters (either rejected_calls or failed_calls).
 * The decision which counter to increment is done using the flags argument, options are:
 * * ERROR_COMMAND_REJECTED - update rejected_calls
 * * ERROR_COMMAND_FAILED - update failed_calls
 *
 * The function also reset the prev_err_count to make sure we will not count the same error
 * twice, its possible to pass a NULL cmd value to indicate that the error was counted elsewhere.
 *
 * The function returns true if stats was updated and false if not. */
int incrCommandStatsOnError(struct serverCommand *cmd, int flags) {
    /* hold the prev error count captured on the last command execution */
    static long long prev_err_count = 0;
    int res = 0;
    if (cmd) {
        if ((server.stat_total_error_replies - prev_err_count) > 0) {
            if (flags & ERROR_COMMAND_REJECTED) {
                cmd->rejected_calls++;
                res = 1;
            } else if (flags & ERROR_COMMAND_FAILED) {
                cmd->failed_calls++;
                res = 1;
            }
        }
    }
    prev_err_count = server.stat_total_error_replies;
    return res;
}

/* Call() is the core of the server's execution of a command.
 *
 * The following flags can be passed:
 * CMD_CALL_NONE        No flags.
 * CMD_CALL_PROPAGATE_AOF   Append command to AOF if it modified the dataset
 *                          or if the client flags are forcing propagation.
 * CMD_CALL_PROPAGATE_REPL  Send command to replicas if it modified the dataset
 *                          or if the client flags are forcing propagation.
 * CMD_CALL_PROPAGATE   Alias for PROPAGATE_AOF|PROPAGATE_REPL.
 * CMD_CALL_FULL        Alias for SLOWLOG|STATS|PROPAGATE.
 *
 * The exact propagation behavior depends on the client flags.
 * Specifically:
 *
 * 1. If the client flags CLIENT_FORCE_AOF or CLIENT_FORCE_REPL are set
 *    and assuming the corresponding CMD_CALL_PROPAGATE_AOF/REPL is set
 *    in the call flags, then the command is propagated even if the
 *    dataset was not affected by the command.
 * 2. If the client flags CLIENT_PREVENT_REPL_PROP or CLIENT_PREVENT_AOF_PROP
 *    are set, the propagation into AOF or to replicas is not performed even
 *    if the command modified the dataset.
 *
 * Note that regardless of the client flags, if CMD_CALL_PROPAGATE_AOF
 * or CMD_CALL_PROPAGATE_REPL are not set, then respectively AOF or
 * replicas propagation will never occur.
 *
 * Client flags are modified by the implementation of a given command
 * using the following API:
 *
 * forceCommandPropagation(client *c, int flags);
 * preventCommandPropagation(client *c);
 * preventCommandAOF(client *c);
 * preventCommandReplication(client *c);
 *
 */
void call(client *c, int flags) {
    long long dirty;
    struct ClientFlags client_old_flags = c->flag;

    struct serverCommand *real_cmd = c->realcmd;
    client *prev_client = server.executing_client;
    server.executing_client = c;

    /* When call() is issued during loading the AOF we don't want commands called
     * from module, exec or LUA to go into the slowlog or to populate statistics. */
    int update_command_stats = !isAOFLoadingContext();

    /* Initialization: clear the flags that must be set by the command on
     * demand, and initialize the array for additional commands propagation. */
    c->flag.force_aof = 0;
    c->flag.force_repl = 0;
    c->flag.prevent_prop = 0;

    /* The server core is in charge of propagation when the first entry point
     * of call() is processCommand().
     * The only other option to get to call() without having processCommand
     * as an entry point is if a module triggers RM_Call outside of call()
     * context (for example, in a timer).
     * In that case, the module is in charge of propagation. */

    /* Call the command. */
    dirty = server.dirty;
    incrCommandStatsOnError(NULL, 0);

    const long long call_timer = ustime();
    enterExecutionUnit(1, call_timer);

    /* setting the CLIENT_EXECUTING_COMMAND flag so we will avoid
     * sending client side caching message in the middle of a command reply.
     * In case of blocking commands, the flag will be un-set only after successfully
     * re-processing and unblock the client.*/
    c->flag.executing_command = 1;

    monotime monotonic_start = 0;
    if (monotonicGetType() == MONOTONIC_CLOCK_HW) monotonic_start = getMonotonicUs();

    c->cmd->proc(c);

    exitExecutionUnit();

    /* In case client is blocked after trying to execute the command,
     * it means the execution is not yet completed and we MIGHT reprocess the command in the future. */
    if (!c->flag.blocked) c->flag.executing_command = 0;

    /* In order to avoid performance implication due to querying the clock using a system call 3 times,
     * we use a monotonic clock, when we are sure its cost is very low, and fall back to non-monotonic call otherwise. */
    ustime_t duration;
    if (monotonicGetType() == MONOTONIC_CLOCK_HW)
        duration = getMonotonicUs() - monotonic_start;
    else
        duration = ustime() - call_timer;

    c->duration += duration;
    dirty = server.dirty - dirty;
    if (dirty < 0) dirty = 0;

    /* Update failed command calls if required. */

    if (!incrCommandStatsOnError(real_cmd, ERROR_COMMAND_FAILED) && c->deferred_reply_errors) {
        /* When call is used from a module client, error stats, and total_error_replies
         * isn't updated since these errors, if handled by the module, are internal,
         * and not reflected to users. however, the commandstats does show these calls
         * (made by RM_Call), so it should log if they failed or succeeded. */
        real_cmd->failed_calls++;
    }

    /* After executing command, we will close the client after writing entire
     * reply if it is set 'CLIENT_CLOSE_AFTER_COMMAND' flag. */
    if (c->flag.close_after_command) {
        c->flag.close_after_command = 0;
        c->flag.close_after_reply = 1;
    }

    /* Clear the original argv.
     * If the client is blocked we will handle slowlog when it is unblocked. */
    if (!c->flag.blocked) freeClientOriginalArgv(c);

    /* Populate the per-command and per-slot statistics that we show in INFO commandstats and CLUSTER SLOT-STATS,
     * respectively. If the client is blocked we will handle latency stats and duration when it is unblocked. */
    if (update_command_stats && !c->flag.blocked) {
        real_cmd->calls++;
        real_cmd->microseconds += c->duration;
    }

    /* The duration needs to be reset after each call except for a blocked command,
     * which is expected to record and reset the duration after unblocking. */
    if (!c->flag.blocked) {
        c->duration = 0;
    }

    /* Propagate the command into the AOF and replication link.
     * We never propagate EXEC explicitly, it will be implicitly
     * propagated if needed (see propagatePendingCommands).
     * Also, module commands take care of themselves */
    if (flags & CMD_CALL_PROPAGATE && !c->flag.prevent_prop && c->cmd->proc != execCommand &&
        !(c->cmd->flags & CMD_MODULE)) {
        int propagate_flags = PROPAGATE_NONE;

        /* Check if the command operated changes in the data set. If so
         * set for replication / AOF propagation. */
        if (dirty) propagate_flags |= (PROPAGATE_AOF | PROPAGATE_REPL);

        /* If the client forced AOF / replication of the command, set
         * the flags regardless of the command effects on the data set. */
        if (c->flag.force_repl) propagate_flags |= PROPAGATE_REPL;
        if (c->flag.force_aof) propagate_flags |= PROPAGATE_AOF;

        /* However prevent AOF / replication propagation if the command
         * implementation called preventCommandPropagation() or similar,
         * or if we don't have the call() flags to do so. */
        if (c->flag.prevent_repl_prop || c->flag.module_prevent_repl_prop || !(flags & CMD_CALL_PROPAGATE_REPL))
            propagate_flags &= ~PROPAGATE_REPL;
        if (c->flag.prevent_aof_prop || c->flag.module_prevent_aof_prop || !(flags & CMD_CALL_PROPAGATE_AOF))
            propagate_flags &= ~PROPAGATE_AOF;

        /* Call alsoPropagate() only if at least one of AOF / replication
         * propagation is needed. */
        if (propagate_flags != PROPAGATE_NONE) alsoPropagate(c->db->id, c->argv, c->argc, propagate_flags);
    }

    /* Restore the old replication flags, since call() can be executed
     * recursively. */
    c->flag.force_aof = client_old_flags.force_aof;
    c->flag.force_repl = client_old_flags.force_repl;
    c->flag.prevent_prop = client_old_flags.prevent_prop;

    if (!c->flag.blocked) {
        /* Modules may call commands in cron, in which case server.current_client
         * is not set. */
        if (server.current_client) {
            server.current_client->commands_processed++;
        }
        server.stat_numcommands++;
    }

    /* Record peak memory after each command and before the eviction that runs
     * before the next command. */
    size_t zmalloc_used = zmalloc_used_memory();
    if (zmalloc_used > server.stat_peak_memory) server.stat_peak_memory = zmalloc_used;

    /* Do some maintenance job and cleanup */
    afterCommand(c);

    /* Client pause takes effect after a transaction has finished. This needs
     * to be located after everything is propagated. */
    if (!server.in_exec && server.client_pause_in_transaction) {
        server.client_pause_in_transaction = 0;
    }

    server.executing_client = prev_client;
}

/* Used when a command that is ready for execution needs to be rejected, due to
 * various pre-execution checks. it returns the appropriate error to the client.
 * If there's a transaction is flags it as dirty, and if the command is EXEC,
 * it aborts the transaction.
 * The duration is reset, since we reject the command, and it did not record.
 * Note: 'reply' is expected to end with \r\n */
void rejectCommand(client *c, robj *reply) {
    flagTransaction(c);
    c->duration = 0;
    if (c->cmd) c->cmd->rejected_calls++;
    if (c->cmd && c->cmd->proc == execCommand) {
        execCommandAbort(c, reply->ptr);
    } else {
        /* using addReplyError* rather than addReply so that the error can be logged. */
        addReplyErrorObject(c, reply);
    }
}

void rejectCommandSds(client *c, sds s) {
    flagTransaction(c);
    c->duration = 0;
    if (c->cmd) c->cmd->rejected_calls++;
    if (c->cmd && c->cmd->proc == execCommand) {
        execCommandAbort(c, s);
        sdsfree(s);
    } else {
        /* The following frees 's'. */
        addReplyErrorSds(c, s);
    }
}

void rejectCommandFormat(client *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sds s = sdscatvprintf(sdsempty(), fmt, ap);
    va_end(ap);
    /* Make sure there are no newlines in the string, otherwise invalid protocol
     * is emitted (The args come from the user, they may contain any character). */
    sdsmapchars(s, "\r\n", "  ", 2);
    rejectCommandSds(c, s);
}

/* This is called after a command in call, we can do some maintenance job in it. */
void afterCommand(client *c) {
    UNUSED(c);
    /* Should be done before trackingHandlePendingKeyInvalidations so that we
     * reply to client before invalidating cache (makes more sense) */
    postExecutionUnitOperations();

    /* Flush other pending push messages. only when we are not in nested call.
     * So the messages are not interleaved with transaction response. */
    if (!server.execution_nesting) listJoin(c->reply, server.pending_push_messages);
}

/* Check if c->cmd exists, fills `err` with details in case it doesn't.
 * Return 1 if exists. */
int commandCheckExistence(client *c, sds *err) {
    if (c->cmd) return 1;
    if (!err) return 0;
    if (isContainerCommandBySds(c->argv[0]->ptr)) {
        /* If we can't find the command but argv[0] by itself is a command
         * it means we're dealing with an invalid subcommand. Print Help. */
        sds cmd = sdsnew((char *)c->argv[0]->ptr);
        sdstoupper(cmd);
        *err = sdsnew(NULL);
        *err = sdscatprintf(*err, "unknown subcommand '%.128s'. Try %s HELP.", (char *)c->argv[1]->ptr, cmd);
        sdsfree(cmd);
    } else {
        sds args = sdsempty();
        int i;
        for (i = 1; i < c->argc && sdslen(args) < 128; i++)
            args = sdscatprintf(args, "'%.*s' ", 128 - (int)sdslen(args), (char *)c->argv[i]->ptr);
        *err = sdsnew(NULL);
        *err =
            sdscatprintf(*err, "unknown command '%.128s', with args beginning with: %s", (char *)c->argv[0]->ptr, args);
        sdsfree(args);
    }
    /* Make sure there are no newlines in the string, otherwise invalid protocol
     * is emitted (The args come from the user, they may contain any character). */
    sdsmapchars(*err, "\r\n", "  ", 2);
    return 0;
}

/* Check if c->argc is valid for c->cmd, fills `err` with details in case it isn't.
 * Return 1 if valid. */
int commandCheckArity(struct serverCommand *cmd, int argc, sds *err) {
    if ((cmd->arity > 0 && cmd->arity != argc) || (argc < -cmd->arity)) {
        if (err) {
            *err = sdsnew(NULL);
            *err = sdscatprintf(*err, "wrong number of arguments for '%s' command", cmd->fullname);
        }
        return 0;
    }

    return 1;
}

/* If we're executing a script, try to extract a set of command flags from
 * it, in case it declared them. Note this is just an attempt, we don't yet
 * know the script command is well formed.*/
uint64_t getCommandFlags(client *c) {
    uint64_t cmd_flags = c->cmd->flags;

    if (c->cmd->proc == fcallCommand || c->cmd->proc == fcallroCommand) {
        cmd_flags = fcallGetCommandFlags(c, cmd_flags);
    } else if (c->cmd->proc == evalCommand || c->cmd->proc == evalRoCommand || c->cmd->proc == evalShaCommand ||
               c->cmd->proc == evalShaRoCommand) {
        cmd_flags = evalGetCommandFlags(c, cmd_flags);
    }

    return cmd_flags;
}

/* If this function gets called we already read a whole
 * command, arguments are in the client argv/argc fields.
 * processCommand() execute the command or prepare the
 * server for a bulk read from the client.
 *
 * If C_OK is returned the client is still alive and valid and
 * other operations can be performed by the caller. Otherwise
 * if C_ERR is returned the client was destroyed (i.e. after QUIT). */
int processCommand(client *c) {
    if (!scriptIsTimedout()) {
        /* Both EXEC and scripts call call() directly so there should be
         * no way in_exec or scriptIsRunning() is 1.
         * That is unless lua_timedout, in which case client may run
         * some commands. */
        serverAssert(!server.in_exec);
        serverAssert(!scriptIsRunning());
    }

    /* in case we are starting to ProcessCommand and we already have a command we assume
     * this is a reprocessing of this command, so we do not want to perform some of the actions again. */
    int client_reprocessing_command = c->cmd ? 1 : 0;

    /* only run command filter if not reprocessing command */
    if (!client_reprocessing_command) {
        moduleCallCommandFilters(c);
    }

    /* Handle possible security attacks. */
    if (!strcasecmp(c->argv[0]->ptr, "host:") || !strcasecmp(c->argv[0]->ptr, "post")) {
        securityWarningCommand(c);
        return C_ERR;
    }

    /* If we're inside a module blocked context yielding that wants to avoid
     * processing clients, postpone the command. */
    if (server.busy_module_yield_flags != BUSY_MODULE_YIELD_NONE &&
        !(server.busy_module_yield_flags & BUSY_MODULE_YIELD_CLIENTS)) {
        blockPostponeClient(c);
        return C_OK;
    }

    /* Now lookup the command and check ASAP about trivial error conditions
     * such as wrong arity, bad command name and so forth.
     * In case we are reprocessing a command after it was blocked,
     * we do not have to repeat the same checks */
    if (!client_reprocessing_command) {
        struct serverCommand *cmd = c->io_parsed_cmd ? c->io_parsed_cmd : lookupCommand(c->argv, c->argc);
        c->cmd = c->lastcmd = c->realcmd = cmd;
        sds err;
        if (!commandCheckExistence(c, &err)) {
            rejectCommandSds(c, err);
            return C_OK;
        }
        if (!commandCheckArity(c->cmd, c->argc, &err)) {
            rejectCommandSds(c, err);
            return C_OK;
        }
    }

    uint64_t cmd_flags = getCommandFlags(c);

    int is_read_command =
        (cmd_flags & CMD_READONLY) || (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_READONLY));
    int is_write_command =
        (cmd_flags & CMD_WRITE) || (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_WRITE));
    int is_denyoom_command =
        (cmd_flags & CMD_DENYOOM) || (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_DENYOOM));
    int is_denyloading_command =
        !(cmd_flags & CMD_LOADING) || (c->cmd->proc == execCommand && (c->mstate.cmd_inv_flags & CMD_LOADING));
    int is_may_replicate_command =
        (cmd_flags & (CMD_WRITE | CMD_MAY_REPLICATE)) ||
        (c->cmd->proc == execCommand && (c->mstate.cmd_flags & (CMD_WRITE | CMD_MAY_REPLICATE)));
    int is_deny_async_loading_command = (cmd_flags & CMD_NO_ASYNC_LOADING) ||
                                        (c->cmd->proc == execCommand && (c->mstate.cmd_flags & CMD_NO_ASYNC_LOADING));
    if (c->flag.multi && c->cmd->flags & CMD_NO_MULTI) {
        rejectCommandFormat(c, "Command not allowed inside a transaction");
        return C_OK;
    }

    /* Disconnect some clients if total clients memory is too high. We do this
     * before key eviction, after the last command was executed and consumed
     * some client output buffer memory. */
    evictClients();
    if (server.current_client == NULL) {
        /* If we evicted ourself then abort processing the command */
        return C_ERR;
    }

    /* Handle the maxmemory directive.
     *
     * Note that we do not want to reclaim memory if we are here re-entering
     * the event loop since there is a busy Lua script running in timeout
     * condition, to avoid mixing the propagation of scripts with the
     * propagation of DELs due to eviction. */
    if (server.maxmemory && !isInsideYieldingLongCommand()) {
        int out_of_memory = (performEvictions() == EVICT_FAIL);

        /* performEvictions may flush replica output buffers. This may result
         * in a replica, that may be the active client, to be freed. */
        if (server.current_client == NULL) return C_ERR;

        if (out_of_memory && is_denyoom_command) {
            rejectCommand(c, shared.oomerr);
            return C_OK;
        }

        /* Save out_of_memory result at command start, otherwise if we check OOM
         * in the first write within script, memory used by lua stack and
         * arguments might interfere. We need to save it for EXEC and module
         * calls too, since these can call EVAL, but avoid saving it during an
         * interrupted / yielding busy script / module. */
        server.pre_command_oom_state = out_of_memory;
    }

    /* Only allow a subset of commands in the context of Pub/Sub if the
     * connection is in RESP2 mode. With RESP3 there are no limits. */
    if ((c->flag.pubsub && c->resp == 2) && c->cmd->proc != pingCommand && c->cmd->proc != subscribeCommand &&
        c->cmd->proc != ssubscribeCommand && c->cmd->proc != unsubscribeCommand &&
        c->cmd->proc != sunsubscribeCommand && c->cmd->proc != psubscribeCommand &&
        c->cmd->proc != punsubscribeCommand && c->cmd->proc != quitCommand && c->cmd->proc != resetCommand) {
        rejectCommandFormat(c,
                            "Can't execute '%s': only (P|S)SUBSCRIBE / "
                            "(P|S)UNSUBSCRIBE / PING / QUIT / RESET are allowed in this context",
                            c->cmd->fullname);
        return C_OK;
    }

    /* Loading DB? Return an error if the command has not the
     * CMD_LOADING flag. */
    if (server.loading && !server.async_loading && is_denyloading_command) {
        rejectCommand(c, shared.loadingerr);
        return C_OK;
    }

    /* During async-loading, block certain commands. */
    if (server.async_loading && is_deny_async_loading_command) {
        rejectCommand(c, shared.loadingerr);
        return C_OK;
    }

    /* when a busy job is being done (script / module)
     * Only allow a limited number of commands.
     * Note that we need to allow the transactions commands, otherwise clients
     * sending a transaction with pipelining without error checking, may have
     * the MULTI plus a few initial commands refused, then the timeout
     * condition resolves, and the bottom-half of the transaction gets
     * executed, see Github PR #7022. */
    if (isInsideYieldingLongCommand() && !(c->cmd->flags & CMD_ALLOW_BUSY)) {
        if (server.busy_module_yield_flags && server.busy_module_yield_reply) {
            rejectCommandFormat(c, "-BUSY %s", server.busy_module_yield_reply);
        } else if (server.busy_module_yield_flags) {
            rejectCommand(c, shared.slowmoduleerr);
        } else if (scriptIsEval()) {
            rejectCommand(c, shared.slowevalerr);
        } else {
            rejectCommand(c, shared.slowscripterr);
        }
        return C_OK;
    }

    /* Prevent a replica from sending commands that access the keyspace.
     * The main objective here is to prevent abuse of client pause check
     * from which replicas are exempt. */
    if (c->flag.replica && (is_may_replicate_command || is_write_command || is_read_command)) {
        rejectCommandFormat(c, "Replica can't interact with the keyspace");
        return C_OK;
    }

    /* If the server is paused, block the client until
     * the pause has ended. Replicas are never paused. */
    if (!c->flag.replica && ((isPausedActions(PAUSE_ACTION_CLIENT_ALL)) ||
                             ((isPausedActions(PAUSE_ACTION_CLIENT_WRITE)) && is_may_replicate_command))) {
        blockPostponeClient(c);
        return C_OK;
    }

    /* Exec the command */
    if (c->flag.multi && c->cmd->proc != execCommand && c->cmd->proc != discardCommand &&
        c->cmd->proc != multiCommand && c->cmd->proc != watchCommand && c->cmd->proc != quitCommand &&
        c->cmd->proc != resetCommand) {
        queueMultiCommand(c, cmd_flags);
        addReply(c, shared.queued);
    } else {
        int flags = CMD_CALL_FULL;
        call(c, flags);
        if (listLength(server.ready_keys) && !isInsideYieldingLongCommand()) handleClientsBlockedOnKeys();
    }
    return C_OK;
}

/* ====================== Error lookup and execution ===================== */

void incrementErrorCount(const char *fullerr, size_t namelen) {
    void *result;
    if (!raxFind(server.errors, (unsigned char *)fullerr, namelen, &result)) {
        struct serverError *error = zmalloc(sizeof(*error));
        error->count = 1;
        raxInsert(server.errors, (unsigned char *)fullerr, namelen, error, NULL);
    } else {
        struct serverError *error = result;
        error->count++;
    }
}

/*================================== Commands =============================== */

/* The PING command. It works in a different way if the client is in
 * in Pub/Sub mode. */
void pingCommand(client *c) {
    /* The command takes zero or one arguments. */
    if (c->argc > 2) {
        addReplyErrorArity(c);
        return;
    }

    if (c->flag.pubsub && c->resp == 2) {
        addReply(c, shared.mbulkhdr[2]);
        addReplyBulkCBuffer(c, "pong", 4);
        if (c->argc == 1)
            addReplyBulkCBuffer(c, "", 0);
        else
            addReplyBulk(c, c->argv[1]);
    } else {
        if (c->argc == 1)
            addReply(c, shared.pong);
        else
            addReplyBulk(c, c->argv[1]);
    }
}

void echoCommand(client *c) {
    addReplyBulk(c, c->argv[1]);
}

void timeCommand(client *c) {
    addReplyArrayLen(c, 2);
    addReplyBulkLongLong(c, server.unixtime);
    addReplyBulkLongLong(c, server.ustime - ((long long)server.unixtime) * 1000000);
}

typedef struct replyFlagNames {
    uint64_t flag;
    const char *name;
} replyFlagNames;

/* Helper function to output flags. */
void addReplyCommandFlags(client *c, uint64_t flags, replyFlagNames *replyFlags) {
    int count = 0, j = 0;
    /* Count them so we don't have to use deferred reply. */
    while (replyFlags[j].name) {
        if (flags & replyFlags[j].flag) count++;
        j++;
    }

    addReplySetLen(c, count);
    j = 0;
    while (replyFlags[j].name) {
        if (flags & replyFlags[j].flag) addReplyStatus(c, replyFlags[j].name);
        j++;
    }
}

void addReplyFlagsForCommand(client *c, struct serverCommand *cmd) {
    replyFlagNames flagNames[] = {{CMD_WRITE, "write"},
                                  {CMD_READONLY, "readonly"},
                                  {CMD_DENYOOM, "denyoom"},
                                  {CMD_MODULE, "module"},
                                  {CMD_ADMIN, "admin"},
                                  {CMD_PUBSUB, "pubsub"},
                                  {CMD_NOSCRIPT, "noscript"},
                                  {CMD_BLOCKING, "blocking"},
                                  {CMD_LOADING, "loading"},
                                  {CMD_STALE, "stale"},
                                  {CMD_SKIP_MONITOR, "skip_monitor"},
                                  {CMD_SKIP_SLOWLOG, "skip_slowlog"},
                                  {CMD_ASKING, "asking"},
                                  {CMD_FAST, "fast"},
                                  {CMD_NO_AUTH, "no_auth"},
                                  /* {CMD_MAY_REPLICATE,     "may_replicate"},, Hidden on purpose */
                                  /* {CMD_SENTINEL,          "sentinel"}, Hidden on purpose */
                                  /* {CMD_ONLY_SENTINEL,     "only_sentinel"}, Hidden on purpose */
                                  {CMD_NO_MANDATORY_KEYS, "no_mandatory_keys"},
                                  /* {CMD_PROTECTED,         "protected"}, Hidden on purpose */
                                  {CMD_NO_ASYNC_LOADING, "no_async_loading"},
                                  {CMD_NO_MULTI, "no_multi"},
                                  {CMD_MOVABLE_KEYS, "movablekeys"},
                                  {CMD_ALLOW_BUSY, "allow_busy"},
                                  /* {CMD_TOUCHES_ARBITRARY_KEYS,  "TOUCHES_ARBITRARY_KEYS"}, Hidden on purpose */
                                  {0, NULL}};
    addReplyCommandFlags(c, cmd->flags, flagNames);
}

void addReplyDocFlagsForCommand(client *c, struct serverCommand *cmd) {
    replyFlagNames docFlagNames[] = {{CMD_DOC_DEPRECATED, "deprecated"}, {CMD_DOC_SYSCMD, "syscmd"}, {0, NULL}};
    addReplyCommandFlags(c, cmd->doc_flags, docFlagNames);
}

void addReplyFlagsForKeyArgs(client *c, uint64_t flags) {
    replyFlagNames docFlagNames[] = {{CMD_KEY_RO, "RO"},
                                     {CMD_KEY_RW, "RW"},
                                     {CMD_KEY_OW, "OW"},
                                     {CMD_KEY_RM, "RM"},
                                     {CMD_KEY_ACCESS, "access"},
                                     {CMD_KEY_UPDATE, "update"},
                                     {CMD_KEY_INSERT, "insert"},
                                     {CMD_KEY_DELETE, "delete"},
                                     {CMD_KEY_NOT_KEY, "not_key"},
                                     {CMD_KEY_INCOMPLETE, "incomplete"},
                                     {CMD_KEY_VARIABLE_FLAGS, "variable_flags"},
                                     {0, NULL}};
    addReplyCommandFlags(c, flags, docFlagNames);
}

/* Must match serverCommandArgType */
const char *ARG_TYPE_STR[] = {
    "string", "integer", "double", "key", "pattern", "unix-time", "pure-token", "oneof", "block",
};

void addReplyFlagsForArg(client *c, uint64_t flags) {
    replyFlagNames argFlagNames[] = {{CMD_ARG_OPTIONAL, "optional"},
                                     {CMD_ARG_MULTIPLE, "multiple"},
                                     {CMD_ARG_MULTIPLE_TOKEN, "multiple_token"},
                                     {0, NULL}};
    addReplyCommandFlags(c, flags, argFlagNames);
}

void addReplyCommandArgList(client *c, struct serverCommandArg *args, int num_args) {
    addReplyArrayLen(c, num_args);
    for (int j = 0; j < num_args; j++) {
        /* Count our reply len so we don't have to use deferred reply. */
        int has_display_text = 1;
        long maplen = 2;
        if (args[j].key_spec_index != -1) maplen++;
        if (args[j].token) maplen++;
        if (args[j].summary) maplen++;
        if (args[j].since) maplen++;
        if (args[j].deprecated_since) maplen++;
        if (args[j].flags) maplen++;
        if (args[j].type == ARG_TYPE_ONEOF || args[j].type == ARG_TYPE_BLOCK) {
            has_display_text = 0;
            maplen++;
        }
        if (has_display_text) maplen++;
        addReplyMapLen(c, maplen);

        addReplyBulkCString(c, "name");
        addReplyBulkCString(c, args[j].name);

        addReplyBulkCString(c, "type");
        addReplyBulkCString(c, ARG_TYPE_STR[args[j].type]);

        if (has_display_text) {
            addReplyBulkCString(c, "display_text");
            addReplyBulkCString(c, args[j].display_text ? args[j].display_text : args[j].name);
        }
        if (args[j].key_spec_index != -1) {
            addReplyBulkCString(c, "key_spec_index");
            addReplyLongLong(c, args[j].key_spec_index);
        }
        if (args[j].token) {
            addReplyBulkCString(c, "token");
            addReplyBulkCString(c, args[j].token);
        }
        if (args[j].summary) {
            addReplyBulkCString(c, "summary");
            addReplyBulkCString(c, args[j].summary);
        }
        if (args[j].since) {
            addReplyBulkCString(c, "since");
            addReplyBulkCString(c, args[j].since);
        }
        if (args[j].deprecated_since) {
            addReplyBulkCString(c, "deprecated_since");
            addReplyBulkCString(c, args[j].deprecated_since);
        }
        if (args[j].flags) {
            addReplyBulkCString(c, "flags");
            addReplyFlagsForArg(c, args[j].flags);
        }
        if (args[j].type == ARG_TYPE_ONEOF || args[j].type == ARG_TYPE_BLOCK) {
            addReplyBulkCString(c, "arguments");
            addReplyCommandArgList(c, args[j].subargs, args[j].num_args);
        }
    }
}

void addReplyCommandHistory(client *c, struct serverCommand *cmd) {
    addReplySetLen(c, cmd->num_history);
    for (int j = 0; j < cmd->num_history; j++) {
        addReplyArrayLen(c, 2);
        addReplyBulkCString(c, cmd->history[j].since);
        addReplyBulkCString(c, cmd->history[j].changes);
    }
}

void addReplyCommandTips(client *c, struct serverCommand *cmd) {
    addReplySetLen(c, cmd->num_tips);
    for (int j = 0; j < cmd->num_tips; j++) {
        addReplyBulkCString(c, cmd->tips[j]);
    }
}

void addReplyCommandKeySpecs(client *c, struct serverCommand *cmd) {
    addReplySetLen(c, cmd->key_specs_num);
    for (int i = 0; i < cmd->key_specs_num; i++) {
        int maplen = 3;
        if (cmd->key_specs[i].notes) maplen++;

        addReplyMapLen(c, maplen);

        if (cmd->key_specs[i].notes) {
            addReplyBulkCString(c, "notes");
            addReplyBulkCString(c, cmd->key_specs[i].notes);
        }

        addReplyBulkCString(c, "flags");
        addReplyFlagsForKeyArgs(c, cmd->key_specs[i].flags);

        addReplyBulkCString(c, "begin_search");
        switch (cmd->key_specs[i].begin_search_type) {
        case KSPEC_BS_UNKNOWN:
            addReplyMapLen(c, 2);
            addReplyBulkCString(c, "type");
            addReplyBulkCString(c, "unknown");

            addReplyBulkCString(c, "spec");
            addReplyMapLen(c, 0);
            break;
        case KSPEC_BS_INDEX:
            addReplyMapLen(c, 2);
            addReplyBulkCString(c, "type");
            addReplyBulkCString(c, "index");

            addReplyBulkCString(c, "spec");
            addReplyMapLen(c, 1);
            addReplyBulkCString(c, "index");
            addReplyLongLong(c, cmd->key_specs[i].bs.index.pos);
            break;
        case KSPEC_BS_KEYWORD:
            addReplyMapLen(c, 2);
            addReplyBulkCString(c, "type");
            addReplyBulkCString(c, "keyword");

            addReplyBulkCString(c, "spec");
            addReplyMapLen(c, 2);
            addReplyBulkCString(c, "keyword");
            addReplyBulkCString(c, cmd->key_specs[i].bs.keyword.keyword);
            addReplyBulkCString(c, "startfrom");
            addReplyLongLong(c, cmd->key_specs[i].bs.keyword.startfrom);
            break;
        default: serverPanic("Invalid begin_search key spec type %d", cmd->key_specs[i].begin_search_type);
        }

        addReplyBulkCString(c, "find_keys");
        switch (cmd->key_specs[i].find_keys_type) {
        case KSPEC_FK_UNKNOWN:
            addReplyMapLen(c, 2);
            addReplyBulkCString(c, "type");
            addReplyBulkCString(c, "unknown");

            addReplyBulkCString(c, "spec");
            addReplyMapLen(c, 0);
            break;
        case KSPEC_FK_RANGE:
            addReplyMapLen(c, 2);
            addReplyBulkCString(c, "type");
            addReplyBulkCString(c, "range");

            addReplyBulkCString(c, "spec");
            addReplyMapLen(c, 3);
            addReplyBulkCString(c, "lastkey");
            addReplyLongLong(c, cmd->key_specs[i].fk.range.lastkey);
            addReplyBulkCString(c, "keystep");
            addReplyLongLong(c, cmd->key_specs[i].fk.range.keystep);
            addReplyBulkCString(c, "limit");
            addReplyLongLong(c, cmd->key_specs[i].fk.range.limit);
            break;
        case KSPEC_FK_KEYNUM:
            addReplyMapLen(c, 2);
            addReplyBulkCString(c, "type");
            addReplyBulkCString(c, "keynum");

            addReplyBulkCString(c, "spec");
            addReplyMapLen(c, 3);
            addReplyBulkCString(c, "keynumidx");
            addReplyLongLong(c, cmd->key_specs[i].fk.keynum.keynumidx);
            addReplyBulkCString(c, "firstkey");
            addReplyLongLong(c, cmd->key_specs[i].fk.keynum.firstkey);
            addReplyBulkCString(c, "keystep");
            addReplyLongLong(c, cmd->key_specs[i].fk.keynum.keystep);
            break;
        default: serverPanic("Invalid find_keys key spec type %d", cmd->key_specs[i].begin_search_type);
        }
    }
}

/* Reply with an array of sub-command using the provided reply callback. */
void addReplyCommandSubCommands(client *c,
                                struct serverCommand *cmd,
                                void (*reply_function)(client *, struct serverCommand *),
                                int use_map) {
    if (!cmd->subcommands_dict) {
        addReplySetLen(c, 0);
        return;
    }

    if (use_map)
        addReplyMapLen(c, dictSize(cmd->subcommands_dict));
    else
        addReplyArrayLen(c, dictSize(cmd->subcommands_dict));
    dictEntry *de;
    dictIterator *di = dictGetSafeIterator(cmd->subcommands_dict);
    while ((de = dictNext(di)) != NULL) {
        struct serverCommand *sub = (struct serverCommand *)dictGetVal(de);
        if (use_map) addReplyBulkCBuffer(c, sub->fullname, sdslen(sub->fullname));
        reply_function(c, sub);
    }
    dictReleaseIterator(di);
}

/* Output the representation of a server command. Used by the COMMAND command and COMMAND INFO. */
void addReplyCommandInfo(client *c, struct serverCommand *cmd) {
    UNUSED(cmd);
    addReplyNull(c);
}

/* Output the representation of a server command. Used by the COMMAND DOCS. */
void addReplyCommandDocs(client *c, struct serverCommand *cmd) {
    /* Count our reply len so we don't have to use deferred reply. */
    long maplen = 1;
    if (cmd->summary) maplen++;
    if (cmd->since) maplen++;
    if (cmd->flags & CMD_MODULE) maplen++;
    if (cmd->complexity) maplen++;
    if (cmd->doc_flags) maplen++;
    if (cmd->deprecated_since) maplen++;
    if (cmd->replaced_by) maplen++;
    if (cmd->history) maplen++;
    if (cmd->args) maplen++;
    if (cmd->subcommands_dict) maplen++;
    addReplyMapLen(c, maplen);

    if (cmd->summary) {
        addReplyBulkCString(c, "summary");
        addReplyBulkCString(c, cmd->summary);
    }
    if (cmd->since) {
        addReplyBulkCString(c, "since");
        addReplyBulkCString(c, cmd->since);
    }

    /* Always have the group, for module commands the group is always "module". */
    addReplyBulkCString(c, "group");
    addReplyBulkCString(c, commandGroupStr(cmd->group));

    if (cmd->complexity) {
        addReplyBulkCString(c, "complexity");
        addReplyBulkCString(c, cmd->complexity);
    }
    if (cmd->flags & CMD_MODULE) {
        addReplyBulkCString(c, "module");
        addReplyBulkCString(c, moduleNameFromCommand(cmd));
    }
    if (cmd->doc_flags) {
        addReplyBulkCString(c, "doc_flags");
        addReplyDocFlagsForCommand(c, cmd);
    }
    if (cmd->deprecated_since) {
        addReplyBulkCString(c, "deprecated_since");
        addReplyBulkCString(c, cmd->deprecated_since);
    }
    if (cmd->replaced_by) {
        addReplyBulkCString(c, "replaced_by");
        addReplyBulkCString(c, cmd->replaced_by);
    }
    if (cmd->history) {
        addReplyBulkCString(c, "history");
        addReplyCommandHistory(c, cmd);
    }
    if (cmd->args) {
        addReplyBulkCString(c, "arguments");
        addReplyCommandArgList(c, cmd->args, cmd->num_args);
    }
    if (cmd->subcommands_dict) {
        addReplyBulkCString(c, "subcommands");
        addReplyCommandSubCommands(c, cmd, addReplyCommandDocs, 1);
    }
}

/* Helper for COMMAND GETKEYS and GETKEYSANDFLAGS */
void getKeysSubcommandImpl(client *c, int with_flags) {
    struct serverCommand *cmd = lookupCommand(c->argv + 2, c->argc - 2);
    getKeysResult result;
    initGetKeysResult(&result);
    int j;

    if (!cmd) {
        addReplyError(c, "Invalid command specified");
        return;
    } else if (!doesCommandHaveKeys(cmd)) {
        addReplyError(c, "The command has no key arguments");
        return;
    } else if ((cmd->arity > 0 && cmd->arity != c->argc - 2) || ((c->argc - 2) < -cmd->arity)) {
        addReplyError(c, "Invalid number of arguments specified for command");
        return;
    }

    if (!getKeysFromCommandWithSpecs(cmd, c->argv + 2, c->argc - 2, GET_KEYSPEC_DEFAULT, &result)) {
        if (cmd->flags & CMD_NO_MANDATORY_KEYS) {
            addReplyArrayLen(c, 0);
        } else {
            addReplyError(c, "Invalid arguments specified for command");
        }
    } else {
        addReplyArrayLen(c, result.numkeys);
        for (j = 0; j < result.numkeys; j++) {
            if (!with_flags) {
                addReplyBulk(c, c->argv[result.keys[j].pos + 2]);
            } else {
                addReplyArrayLen(c, 2);
                addReplyBulk(c, c->argv[result.keys[j].pos + 2]);
                addReplyFlagsForKeyArgs(c, result.keys[j].flags);
            }
        }
    }
    getKeysFreeResult(&result);
}

/* COMMAND GETKEYSANDFLAGS cmd arg1 arg2 ... */
void commandGetKeysAndFlagsCommand(client *c) {
    getKeysSubcommandImpl(c, 1);
}

/* COMMAND GETKEYS cmd arg1 arg2 ... */
void getKeysSubcommand(client *c) {
    getKeysSubcommandImpl(c, 0);
}

/* COMMAND (no args) */
void commandCommand(client *c) {
    dictIterator *di;
    dictEntry *de;

    addReplyArrayLen(c, dictSize(server.commands));
    di = dictGetIterator(server.commands);
    while ((de = dictNext(di)) != NULL) {
        addReplyCommandInfo(c, dictGetVal(de));
    }
    dictReleaseIterator(di);
}

/* COMMAND COUNT */
void commandCountCommand(client *c) {
    addReplyLongLong(c, dictSize(server.commands));
}

typedef enum {
    COMMAND_LIST_FILTER_MODULE,
    COMMAND_LIST_FILTER_ACLCAT,
    COMMAND_LIST_FILTER_PATTERN,
} commandListFilterType;

typedef struct {
    commandListFilterType type;
    sds arg;
    struct {
        int valid;
        union {
            uint64_t aclcat;
            void *module_handle;
        } u;
    } cache;
} commandListFilter;

int shouldFilterFromCommandList(struct serverCommand *cmd, commandListFilter *filter) {
    switch (filter->type) {
    case (COMMAND_LIST_FILTER_MODULE):
        if (!filter->cache.valid) {
            filter->cache.u.module_handle = moduleGetHandleByName(filter->arg);
            filter->cache.valid = 1;
        }
        return !moduleIsModuleCommand(filter->cache.u.module_handle, cmd);
    case (COMMAND_LIST_FILTER_PATTERN):
        return !stringmatchlen(filter->arg, sdslen(filter->arg), cmd->fullname, sdslen(cmd->fullname), 1);
    default: serverPanic("Invalid filter type %d", filter->type);
    }
}

/* COMMAND LIST FILTERBY (MODULE <module-name>|ACLCAT <cat>|PATTERN <pattern>) */
void commandListWithFilter(client *c, dict *commands, commandListFilter filter, int *numcmds) {
    dictEntry *de;
    dictIterator *di = dictGetIterator(commands);

    while ((de = dictNext(di)) != NULL) {
        struct serverCommand *cmd = dictGetVal(de);
        if (!shouldFilterFromCommandList(cmd, &filter)) {
            addReplyBulkCBuffer(c, cmd->fullname, sdslen(cmd->fullname));
            (*numcmds)++;
        }

        if (cmd->subcommands_dict) {
            commandListWithFilter(c, cmd->subcommands_dict, filter, numcmds);
        }
    }
    dictReleaseIterator(di);
}

/* COMMAND LIST */
void commandListWithoutFilter(client *c, dict *commands, int *numcmds) {
    dictEntry *de;
    dictIterator *di = dictGetIterator(commands);

    while ((de = dictNext(di)) != NULL) {
        struct serverCommand *cmd = dictGetVal(de);
        addReplyBulkCBuffer(c, cmd->fullname, sdslen(cmd->fullname));
        (*numcmds)++;

        if (cmd->subcommands_dict) {
            commandListWithoutFilter(c, cmd->subcommands_dict, numcmds);
        }
    }
    dictReleaseIterator(di);
}

/* COMMAND LIST [FILTERBY (MODULE <module-name>|ACLCAT <cat>|PATTERN <pattern>)] */
void commandListCommand(client *c) {
    /* Parse options. */
    int i = 2, got_filter = 0;
    commandListFilter filter = {0};
    for (; i < c->argc; i++) {
        int moreargs = (c->argc - 1) - i; /* Number of additional arguments. */
        char *opt = c->argv[i]->ptr;
        if (!strcasecmp(opt, "filterby") && moreargs == 2) {
            char *filtertype = c->argv[i + 1]->ptr;
            if (!strcasecmp(filtertype, "module")) {
                filter.type = COMMAND_LIST_FILTER_MODULE;
            } else if (!strcasecmp(filtertype, "aclcat")) {
                filter.type = COMMAND_LIST_FILTER_ACLCAT;
            } else if (!strcasecmp(filtertype, "pattern")) {
                filter.type = COMMAND_LIST_FILTER_PATTERN;
            } else {
                addReplyErrorObject(c, shared.syntaxerr);
                return;
            }
            got_filter = 1;
            filter.arg = c->argv[i + 2]->ptr;
            i += 2;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }

    int numcmds = 0;
    void *replylen = addReplyDeferredLen(c);

    if (got_filter) {
        commandListWithFilter(c, server.commands, filter, &numcmds);
    } else {
        commandListWithoutFilter(c, server.commands, &numcmds);
    }

    setDeferredArrayLen(c, replylen, numcmds);
}

/* COMMAND INFO [<command-name> ...] */
void commandInfoCommand(client *c) {
    int i;

    if (c->argc == 2) {
        dictIterator *di;
        dictEntry *de;
        addReplyArrayLen(c, dictSize(server.commands));
        di = dictGetIterator(server.commands);
        while ((de = dictNext(di)) != NULL) {
            addReplyCommandInfo(c, dictGetVal(de));
        }
        dictReleaseIterator(di);
    } else {
        addReplyArrayLen(c, c->argc - 2);
        for (i = 2; i < c->argc; i++) {
            addReplyCommandInfo(c, lookupCommandBySds(c->argv[i]->ptr));
        }
    }
}

/* COMMAND DOCS [command-name [command-name ...]] */
void commandDocsCommand(client *c) {
    int i;
    if (c->argc == 2) {
        /* Reply with an array of all commands */
        dictIterator *di;
        dictEntry *de;
        addReplyMapLen(c, dictSize(server.commands));
        di = dictGetIterator(server.commands);
        while ((de = dictNext(di)) != NULL) {
            struct serverCommand *cmd = dictGetVal(de);
            addReplyBulkCBuffer(c, cmd->fullname, sdslen(cmd->fullname));
            addReplyCommandDocs(c, cmd);
        }
        dictReleaseIterator(di);
    } else {
        /* Reply with an array of the requested commands (if we find them) */
        int numcmds = 0;
        void *replylen = addReplyDeferredLen(c);
        for (i = 2; i < c->argc; i++) {
            struct serverCommand *cmd = lookupCommandBySds(c->argv[i]->ptr);
            if (!cmd) continue;
            addReplyBulkCBuffer(c, cmd->fullname, sdslen(cmd->fullname));
            addReplyCommandDocs(c, cmd);
            numcmds++;
        }
        setDeferredMapLen(c, replylen, numcmds);
    }
}

/* COMMAND GETKEYS arg0 arg1 arg2 ... */
void commandGetKeysCommand(client *c) {
    getKeysSubcommand(c);
}

/* COMMAND HELP */
void commandHelpCommand(client *c) {
    /* clang-format off */
    const char *help[] = {
"(no subcommand)",
"    Return details about all commands.",
"COUNT",
"    Return the total number of commands in this server.",
"LIST",
"    Return a list of all commands in this server.",
"INFO [<command-name> ...]",
"    Return details about multiple commands.",
"    If no command names are given, documentation details for all",
"    commands are returned.",
"DOCS [<command-name> ...]",
"    Return documentation details about multiple commands.",
"    If no command names are given, documentation details for all",
"    commands are returned.",
"GETKEYS <full-command>",
"    Return the keys from a full command.",
"GETKEYSANDFLAGS <full-command>",
"    Return the keys and the access flags from a full command.",
NULL
    };
    /* clang-format on */
    addReplyHelp(c, help);
}

/* Convert an amount of bytes into a human readable string in the form
 * of 100B, 2G, 100M, 4K, and so forth. */
void bytesToHuman(char *s, size_t size, unsigned long long n) {
    double d;

    if (n < 1024) {
        /* Bytes */
        snprintf(s, size, "%lluB", n);
    } else if (n < (1024 * 1024)) {
        d = (double)n / (1024);
        snprintf(s, size, "%.2fK", d);
    } else if (n < (1024LL * 1024 * 1024)) {
        d = (double)n / (1024 * 1024);
        snprintf(s, size, "%.2fM", d);
    } else if (n < (1024LL * 1024 * 1024 * 1024)) {
        d = (double)n / (1024LL * 1024 * 1024);
        snprintf(s, size, "%.2fG", d);
    } else if (n < (1024LL * 1024 * 1024 * 1024 * 1024)) {
        d = (double)n / (1024LL * 1024 * 1024 * 1024);
        snprintf(s, size, "%.2fT", d);
    } else if (n < (1024LL * 1024 * 1024 * 1024 * 1024 * 1024)) {
        d = (double)n / (1024LL * 1024 * 1024 * 1024 * 1024);
        snprintf(s, size, "%.2fP", d);
    } else {
        /* Let's hope we never need this */
        snprintf(s, size, "%lluB", n);
    }
}

const char *replstateToString(int replstate) {
    switch (replstate) {
    case REPLICA_STATE_WAIT_BGSAVE_START:
    case REPLICA_STATE_WAIT_BGSAVE_END: return "wait_bgsave";
    case REPLICA_STATE_BG_RDB_LOAD: return "bg_transfer";
    case REPLICA_STATE_SEND_BULK: return "send_bulk";
    case REPLICA_STATE_ONLINE: return "online";
    default: return "";
    }
}

/* Characters we sanitize on INFO output to maintain expected format. */
static char unsafe_info_chars[] = "#:\n\r";
static char unsafe_info_chars_substs[] = "____"; /* Must be same length as above */

/* Returns a sanitized version of s that contains no unsafe info string chars.
 * If no unsafe characters are found, simply returns s. Caller needs to
 * free tmp if it is non-null on return.
 */
const char *getSafeInfoString(const char *s, size_t len, char **tmp) {
    *tmp = NULL;
    if (mempbrk(s, len, unsafe_info_chars, sizeof(unsafe_info_chars) - 1) == NULL) return s;
    char *new = *tmp = zmalloc(len + 1);
    memcpy(new, s, len);
    new[len] = '\0';
    return memmapchars(new, len, unsafe_info_chars, unsafe_info_chars_substs, sizeof(unsafe_info_chars) - 1);
}

/* Takes a null terminated sections list, and adds them to the dict. */
void addInfoSectionsToDict(dict *section_dict, char **sections) {
    while (*sections) {
        sds section = sdsnew(*sections);
        if (dictAdd(section_dict, section, NULL) == DICT_ERR) sdsfree(section);
        sections++;
    }
}

/* Cached copy of the default sections, as an optimization. */
static dict *cached_default_info_sections = NULL;

void releaseInfoSectionDict(dict *sec) {
    if (sec != cached_default_info_sections) dictRelease(sec);
}

/* Create a dictionary with unique section names to be used by genValkeyInfoString.
 * 'argv' and 'argc' are list of arguments for INFO.
 * 'defaults' is an optional null terminated list of default sections.
 * 'out_all' and 'out_everything' are optional.
 * The resulting dictionary should be released with releaseInfoSectionDict. */
dict *genInfoSectionDict(robj **argv, int argc, char **defaults, int *out_all, int *out_everything) {
    char *default_sections[] = {"server", "clients",     "memory",     "persistence", "stats",    "replication",
                                "cpu",    "module_list", "errorstats", "cluster",     "keyspace", NULL};
    if (!defaults) defaults = default_sections;

    if (argc == 0) {
        /* In this case we know the dict is not gonna be modified, so we cache
         * it as an optimization for a common case. */
        if (cached_default_info_sections) return cached_default_info_sections;
        cached_default_info_sections = dictCreate(&stringSetDictType);
        dictExpand(cached_default_info_sections, 16);
        addInfoSectionsToDict(cached_default_info_sections, defaults);
        return cached_default_info_sections;
    }

    dict *section_dict = dictCreate(&stringSetDictType);
    dictExpand(section_dict, min(argc, 16));
    for (int i = 0; i < argc; i++) {
        if (!strcasecmp(argv[i]->ptr, "default")) {
            addInfoSectionsToDict(section_dict, defaults);
        } else if (!strcasecmp(argv[i]->ptr, "all")) {
            if (out_all) *out_all = 1;
        } else if (!strcasecmp(argv[i]->ptr, "everything")) {
            if (out_everything) *out_everything = 1;
            if (out_all) *out_all = 1;
        } else {
            sds section = sdsnew(argv[i]->ptr);
            if (dictAdd(section_dict, section, NULL) != DICT_OK) sdsfree(section);
        }
    }
    return section_dict;
}

/* sets blocking_keys to the total number of keys which has at least one client blocked on them.
 * sets blocking_keys_on_nokey to the total number of keys which has at least one client
 * blocked on them to be written or deleted.
 * sets watched_keys to the total number of keys which has at least on client watching on them. */
void totalNumberOfStatefulKeys(unsigned long *blocking_keys,
                               unsigned long *blocking_keys_on_nokey,
                               unsigned long *watched_keys) {
    unsigned long bkeys = 0, bkeys_on_nokey = 0, wkeys = 0;
    for (int j = 0; j < server.dbnum; j++) {
        bkeys += dictSize(server.db[j].blocking_keys);
        bkeys_on_nokey += dictSize(server.db[j].blocking_keys_unblock_on_nokey);
        wkeys += dictSize(server.db[j].watched_keys);
    }
    if (blocking_keys) *blocking_keys = bkeys;
    if (blocking_keys_on_nokey) *blocking_keys_on_nokey = bkeys_on_nokey;
    if (watched_keys) *watched_keys = wkeys;
}

void monitorCommand(client *c) {
    if (c->flag.deny_blocking) {
        /**
         * A client that has CLIENT_DENY_BLOCKING flag on
         * expects a reply per command and so can't execute MONITOR. */
        addReplyError(c, "MONITOR isn't allowed for DENY BLOCKING client");
        return;
    }

    /* ignore MONITOR if already replica or in monitor mode */
    if (c->flag.replica) return;

    c->flag.replica = 1;
    c->flag.monitor = 1;
    listAddNodeTail(server.monitors, c);
    addReply(c, shared.ok);
}

/* =================================== Main! ================================ */
sds getVersion(void) {
    sds version = sdscatprintf(sdsempty(), "v=%s sha=%s:%d malloc=%s bits=%d build=%llx", VALKEY_VERSION,
                               serverGitSHA1(), atoi(serverGitDirty()) > 0, ZMALLOC_LIB, sizeof(long) == 4 ? 32 : 64,
                               (unsigned long long)serverBuildId());
    return version;
}

void usage(void) {
    fprintf(stderr, "Usage: ./valkey-server [/path/to/valkey.conf] [options] [-]\n");
    fprintf(stderr, "       ./valkey-server - (read config from stdin)\n");
    fprintf(stderr, "       ./valkey-server -v or --version\n");
    fprintf(stderr, "       ./valkey-server -h or --help\n");
    fprintf(stderr, "       ./valkey-server --test-memory <megabytes>\n");
    fprintf(stderr, "       ./valkey-server --check-system\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "       ./valkey-server (run the server with default conf)\n");
    fprintf(stderr, "       echo 'maxmemory 128mb' | ./valkey-server -\n");
    fprintf(stderr, "       ./valkey-server /etc/valkey/6379.conf\n");
    fprintf(stderr, "       ./valkey-server --port 7777\n");
    fprintf(stderr, "       ./valkey-server --port 7777 --replicaof 127.0.0.1 8888\n");
    fprintf(stderr, "       ./valkey-server /etc/myvalkey.conf --loglevel verbose -\n");
    fprintf(stderr, "       ./valkey-server /etc/myvalkey.conf --loglevel verbose\n\n");
    fprintf(stderr, "Sentinel mode:\n");
    fprintf(stderr, "       ./valkey-server /etc/sentinel.conf --sentinel\n");
    exit(1);
}

void serverOutOfMemoryHandler(size_t allocation_size) {
    serverLog(LL_WARNING, "Out Of Memory allocating %zu bytes!", allocation_size);
    serverPanic("Valkey aborting for OUT OF MEMORY. Allocating %zu bytes!", allocation_size);
}

int iAmPrimary(void) {
    // return (server.primary_host == NULL);
    return 1;
}

int main(int argc, char **argv) {
    struct timeval tv;
    int j;
    char config_from_stdin = 0;

    tzset(); /* Populates 'timezone' global. */
    zmalloc_set_oom_handler(serverOutOfMemoryHandler);

    /* To achieve entropy, in case of containers, their time() and getpid() can
     * be the same. But value of tv_usec is fast enough to make the difference */
    gettimeofday(&tv, NULL);
    srand(time(NULL) ^ getpid() ^ tv.tv_usec);
    srandom(time(NULL) ^ getpid() ^ tv.tv_usec);
    init_genrand64(((long long)tv.tv_sec * 1000000 + tv.tv_usec) ^ getpid());
    crc64_init();

    /* Store umask value. Because umask(2) only offers a set-and-get API we have
     * to reset it and restore it back. We do this early to avoid a potential
     * race condition with threads that could be creating files or directories.
     */
    umask(server.umask = umask(0777));

    uint8_t hashseed[16];
    getRandomBytes(hashseed, sizeof(hashseed));
    dictSetHashFunctionSeed(hashseed);

    char *exec_name = strrchr(argv[0], '/');
    if (exec_name == NULL) exec_name = argv[0];
    initServerConfig();
    moduleInitModulesSystem();
    connTypeInitialize();

    /* Store the executable path and arguments in a safe place in order
     * to be able to restart the server later. */
    server.executable = getAbsolutePath(argv[0]);
    server.exec_argv = zmalloc(sizeof(char *) * (argc + 1));
    server.exec_argv[argc] = NULL;
    for (j = 0; j < argc; j++) server.exec_argv[j] = zstrdup(argv[j]);

    if (argc >= 2) {
        sds options = sdsempty();
        loadServerConfig(server.configfile, config_from_stdin, options);
        sdsfree(options);
    }

    serverLog(LL_NOTICE, "oO0OoO0OoO0Oo Valkey is starting oO0OoO0OoO0Oo");
    serverLog(LL_NOTICE, "Valkey version=%s, bits=%d, commit=%s, modified=%d, pid=%d, just started", VALKEY_VERSION,
              (sizeof(long) == 8) ? 64 : 32, serverGitSHA1(), strtol(serverGitDirty(), NULL, 10) > 0, (int)getpid());

    if (argc == 1) {
        serverLog(LL_WARNING,
                  "Warning: no config file specified, using the default config. In order to specify a config file use "
                  "%s /path/to/valkey.conf",
                  argv[0]);
    } else {
        serverLog(LL_NOTICE, "Configuration loaded");
    }

    initServer();
    moduleInitModulesSystemLast();
    moduleLoadFromQueue();
    InitServerLast();

    /* Things not needed when running in Sentinel mode. */
    serverLog(LL_NOTICE, "Server initialized");
    /* Warning the user about suspicious maxmemory setting. */
    if (server.maxmemory > 0 && server.maxmemory < 1024 * 1024) {
        serverLog(LL_WARNING,
                  "WARNING: You specified a maxmemory value that is less than 1MB (current value is %llu bytes). Are "
                  "you sure this is what you really want?",
                  server.maxmemory);
    }

    setOOMScoreAdj(-1);
    return 0;
}

/* The End */
