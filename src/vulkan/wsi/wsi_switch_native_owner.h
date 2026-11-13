/* SPDX-License-Identifier: MIT
 * VI2 release dependencies and acquire deadlines for the native WSI.
 *
 * The WSI sends its VI2 requests through libounce-nx's VI2 API
 * (vi2GetLayerAvailability, vi2WaitLayerAvailability, vi2PresentLayer;
 * see nvk_wsi.c). This file keeps only the decoding of the 16-byte release
 * dependency that a present returns, and the acquire deadline.
 */
#ifndef WSI_SWITCH_NATIVE_OWNER_H
#define WSI_SWITCH_NATIVE_OWNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The release dependency of a presented image: a 32-bit producer token,
 * a mode byte, three padding bytes and a 64-bit counter threshold, little
 * endian. */
struct wsn_dependency { uint8_t bytes[16]; };
uint32_t wsn_dependency_token(const struct wsn_dependency *d);
uint8_t wsn_dependency_mode(const struct wsn_dependency *d);
uint64_t wsn_dependency_threshold(const struct wsn_dependency *d);
/* Token, mode and threshold are all zero: no release to wait for. */
bool wsn_dependency_semantic_empty(const struct wsn_dependency *d);
/* Mode 0 or 1 and zero padding bytes (an OunceVK policy). */
bool wsn_dependency_canonical(const struct wsn_dependency *d);

/* The caller gives monotonic time. One deadline covers all later acquire
 * phases. A finite timeout that overflows saturates. It never becomes the
 * infinite-timeout value. */
struct wsn_deadline { uint64_t end; bool infinite; };
struct wsn_deadline wsn_deadline_make(uint64_t now, uint64_t timeout);
uint64_t wsn_deadline_remaining(struct wsn_deadline deadline, uint64_t now);

#endif
