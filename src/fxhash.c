/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* C port of the rustc-hash FxHasher (Orson Peters polynomial hash). See
 * fxhash.h for the security constraints.
 *
 * Pipeline for a byte string:
 *   1. fxHashBytes() compresses the whole string into one u64. It seeds two
 *      state words with fixed constants, handles the first 16 bytes with a
 *      length-based fast path, mixes longer inputs in 16-byte chunks using
 *      fxMultiplyMix(), and folds the length into the result.
 *   2. fxFinish() applies one multiplicative-congruence step with K and
 *      rotates the high-entropy top bits down into the bucket-selecting low
 *      bits.
 * All arithmetic is unsigned with natural wraparound. */

#include "fmacros.h"
#include "fxhash.h"
#include "endianconv.h"

#include <string.h>

/* Multiplicative-congruence constant, chosen as a spectrally good MCG
 * multiplier (Steele and Vigna). */
#define FXHASH_K 0xf1357aea2e62a9c5ULL

/* Fixed hash_bytes seeds; "nothing special, digits of pi". */
#define FXHASH_SEED1 0x243f6a8885a308d3ULL
#define FXHASH_SEED2 0x13198a2e03707344ULL

/* XOR'ed into the second bulk word so all-zero inputs do not collapse. */
#define FXHASH_PREVENT_TRIVIAL_ZERO_COLLAPSE 0xa4093822299f31d0ULL

/* Bucket indices are taken from the low bits, but a multiplicative hash has
 * the most entropy in the top bits, so rotate them down before finishing. */
#define FXHASH_FINISH_ROTATE 26

static inline uint64_t fxRotl(uint64_t x, int shift) {
    return (x << shift) | (x >> (64 - shift));
}

/* Locale-independent A-Z to a-z folding, matching siptlw() in siphash.c. */
static inline int fxTolower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

/* Middle-bits multiplication mix. On 64-bit targets with widening multiply
 * this is one u64*u64->u128 product; the XOR of its halves fluctuates across
 * all output bits. On targets without 128-bit products it falls back to a
 * 32-bit decomposition with the same avalanche goal. */
static inline uint64_t fxMultiplyMix(uint64_t x, uint64_t y) {
#if defined(__SIZEOF_INT128__)
    __uint128_t full = (__uint128_t)x * (__uint128_t)y;
    return (uint64_t)full ^ (uint64_t)(full >> 64);
#else
    uint32_t lx = (uint32_t)x;
    uint32_t ly = (uint32_t)y;
    uint32_t hx = (uint32_t)(x >> 32);
    uint32_t hy = (uint32_t)(y >> 32);
    uint64_t afull = (uint64_t)lx * (uint64_t)hy;
    uint64_t bfull = (uint64_t)hx * (uint64_t)ly;
    return afull ^ ((bfull >> 32) | (bfull << 32));
#endif
}

static inline uint64_t fxLoad64(const uint8_t *p, int case_fold) {
    uint64_t word;
    if (case_fold) {
        uint8_t buf[8];
        int i;
        for (i = 0; i < 8; i++) buf[i] = (uint8_t)fxTolower(p[i]);
        memcpy(&word, buf, sizeof(word));
    } else {
        memcpy(&word, p, sizeof(word));
    }
    memrev64ifbe(&word);
    return word;
}

static inline uint64_t fxLoad32(const uint8_t *p, int case_fold) {
    uint32_t word;
    if (case_fold) {
        uint8_t buf[4];
        int i;
        for (i = 0; i < 4; i++) buf[i] = (uint8_t)fxTolower(p[i]);
        memcpy(&word, buf, sizeof(word));
    } else {
        memcpy(&word, p, sizeof(word));
    }
    memrev32ifbe(&word);
    return (uint64_t)word;
}

/* Compress a byte string to one u64. Mirrors hash_bytes() in rustc-hash. */
static uint64_t fxHashBytes(const uint8_t *in, size_t len, int case_fold) {
    uint64_t s0 = FXHASH_SEED1;
    uint64_t s1 = FXHASH_SEED2;

    if (len <= 16) {
        if (len >= 8) {
            s0 ^= fxLoad64(in, case_fold);
            s1 ^= fxLoad64(in + len - 8, case_fold);
        } else if (len >= 4) {
            s0 ^= fxLoad32(in, case_fold);
            s1 ^= fxLoad32(in + len - 4, case_fold);
        } else if (len > 0) {
            int lo = case_fold ? fxTolower(in[0]) : in[0];
            int mid = case_fold ? fxTolower(in[len / 2]) : in[len / 2];
            int hi = case_fold ? fxTolower(in[len - 1]) : in[len - 1];
            s0 ^= (uint64_t)lo;
            s1 ^= (uint64_t)((hi << 8) | mid);
        }
    } else {
        /* Bulk over all but the last byte, then mix the final 16-byte suffix
         * (the bulk loop and suffix may partially overlap, by design). */
        size_t bulk_len = len - 1;
        size_t i = 0;
        while (i + 16 <= bulk_len) {
            uint64_t x = fxLoad64(in + i, case_fold);
            uint64_t y = fxLoad64(in + i + 8, case_fold);
            uint64_t t = fxMultiplyMix(s0 ^ x, FXHASH_PREVENT_TRIVIAL_ZERO_COLLAPSE ^ y);
            s0 = s1;
            s1 = t;
            i += 16;
        }
        s0 ^= fxLoad64(in + len - 16, case_fold);
        s1 ^= fxLoad64(in + len - 8, case_fold);
    }

    return fxMultiplyMix(s0, s1) ^ (uint64_t)len;
}

/* add_to_hash() starting from the default zero state, followed by finish(). */
static inline uint64_t fxFinish(uint64_t compressed) {
    uint64_t hash = compressed * FXHASH_K;
    return fxRotl(hash, FXHASH_FINISH_ROTATE);
}

uint64_t fxHash64(const uint8_t *in, size_t len) {
    return fxFinish(fxHashBytes(in, len, 0));
}

uint64_t fxHash64Case(const uint8_t *in, size_t len) {
    return fxFinish(fxHashBytes(in, len, 1));
}
