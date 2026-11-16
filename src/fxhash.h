/*
 * Copyright Valkey Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __FXHASH_H
#define __FXHASH_H

#include <stddef.h>
#include <stdint.h>

/* C port of the current rustc-hash FxHasher (the Orson Peters polynomial
 * hash, see https://github.com/rust-lang/rustc-hash). A byte string is first
 * compressed to a single u64 by a wyhash-inspired hash_bytes() step (fixed
 * seeds, length mixed in), then run through one multiplicative-congruence
 * step and a left rotate that moves the high-entropy top bits into the low
 * bits used for bucket selection.
 *
 * It is non-cryptographic, keyless and offers no resistance to adversarial
 * inputs, so it must only back hash tables with trusted, internally generated
 * keys. Table lookups always consult the key comparison callback, so
 * collisions cannot affect correctness.
 *
 * Both functions are deterministic across architectures: input bytes are
 * interpreted as little-endian words regardless of host byte order. The empty
 * string hashes to a fixed non-zero value because the fixed seeds participate. */

uint64_t fxHash64(const uint8_t *in, size_t len);

/* Same algorithm, with locale-independent A-Z to a-z folding applied while
 * loading each byte, for tables whose keyCompare is case-insensitive. */
uint64_t fxHash64Case(const uint8_t *in, size_t len);

#endif /* __FXHASH_H */
