/* SPDX-License-Identifier: MIT */

#ifndef NVKMD_NVGPU_NATIVE_PROFILE_H
#define NVKMD_NVGPU_NATIVE_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Native ioctl3 payloads, not libnx's embedded characteristics structure. */
#define NVKMD_NVGPU_NATIVE_CHARACTERISTICS_IOCTL UINT32_C(0xc0e84705)
#define NVKMD_NVGPU_NATIVE_CHARACTERISTICS_ARG_SIZE 0xe8
#define NVKMD_NVGPU_NATIVE_CHARACTERISTICS_SIZE 0xd8

/* Diagnostic observation only. This is not nv_device_info, on purpose. All
 * fields keep their wire widths and units. Unknown bytes stay in raw[].
 * Nothing here derives a compiler SM, CPU cache granule, memory budget or
 * Vulkan capability. The caller must keep the query source and the owner
 * history. This value does not own service handles and does not guarantee
 * their lifetime.
 */
struct nvkmd_nvgpu_native_characteristics {
   uint8_t raw[NVKMD_NVGPU_NATIVE_CHARACTERISTICS_SIZE];
   uint32_t arch;
   uint32_t impl;
   uint32_t revision;
   uint32_t gpc_count;
   uint64_t gpu_l2_size_B;
   uint64_t memory_raw;
   uint32_t tpc_per_gpc;
   uint32_t q24_raw;
   uint32_t big_page_size_raw;
   uint32_t compression_page_size_raw;
   uint32_t pde_coverage_raw;
   uint32_t big_page_mask_raw;
   uint32_t gpc_mask;
   uint32_t native_version_a;
   uint32_t native_version_b;
   uint32_t warp_count_raw;
   uint8_t gpu_va_bits;
   uint64_t flags;
   uint32_t class_2d;
   uint32_t class_3d;
   uint32_t class_compute;
   uint32_t class_gpfifo;
   uint32_t class_inline;
   uint32_t class_copy;
   int16_t versions[8];
   uint32_t gpc_count_copy;
};

/* Private metadata about a decoded value. It is never nv_device_info or a
 * readiness test. RAW means only a decoded wire scalar. STATIC_CANDIDATE
 * means a known policy, not a validated engine or a guarantee of native
 * execution. Zero initialization marks every value as unresolved.
 */
enum nvkmd_nvgpu_native_policy_validity {
   NVKMD_NVGPU_NATIVE_POLICY_UNRESOLVED = 0,
   NVKMD_NVGPU_NATIVE_POLICY_RAW,
   NVKMD_NVGPU_NATIVE_POLICY_STATIC_CANDIDATE,
};

struct nvkmd_nvgpu_native_policy_scalar {
   uint32_t value;
   enum nvkmd_nvgpu_native_policy_validity validity;
};

#define NVKMD_NVGPU_NATIVE_C7C0_CARVEOUT_COUNT 6

struct nvkmd_nvgpu_native_resource_policy {
   struct nvkmd_nvgpu_native_policy_scalar q3c, q40, q44;
   struct nvkmd_nvgpu_native_policy_scalar compute_class_raw;
   /* SM/compiler target remains unknown. SPA is only the model-808 static
    * packet candidate, independently derived from Q3c and distinct from Q40
    * even when both values happen to be 806. No engine acceptance or command
    * submission follows this observation.
    */
   struct nvkmd_nvgpu_native_policy_scalar sm_version, spa_version;
   struct nvkmd_nvgpu_native_policy_scalar engine_class, shader_isa;
   struct nvkmd_nvgpu_native_policy_scalar cpu_cache_granule_B;
   struct nvkmd_nvgpu_native_policy_scalar max_workgroup_shared_B;
   struct nvkmd_nvgpu_native_policy_scalar max_workgroup_threads;
   struct {
      enum nvkmd_nvgpu_native_policy_validity validity;
      uint32_t compute_class;
      uint32_t carveout_count;
      uint32_t carveouts_B[NVKMD_NVGPU_NATIVE_C7C0_CARVEOUT_COUNT];
      uint32_t shared_rounding_B;
      uint32_t threads_per_warp;
      uint32_t resident_warps;
   } scheduling;
};

/* chars must be a successfully decoded observation. The function keeps the
 * raw words, also for unsupported inputs. It adds the static scheduling and
 * SPA packet policy only when the profile screen returns UNRESOLVED. It does
 * not infer a runtime engine from Q60. It does not claim that the gated
 * class vector was queried. A NULL chars clears out. A NULL out is ignored.
 * The stored result has the same owner history as chars.
 */
void nvkmd_nvgpu_native_resource_policy_decode(
   const struct nvkmd_nvgpu_native_characteristics *chars,
   struct nvkmd_nvgpu_native_resource_policy *out);

struct nvkmd_nvgpu_native_query_results {
   uint32_t transport;
   uint32_t service;
   uint32_t driver;
};

enum nvkmd_nvgpu_native_decode_status {
   NVKMD_NVGPU_NATIVE_DECODED,
   NVKMD_NVGPU_NATIVE_INVALID_ARGUMENT,
   NVKMD_NVGPU_NATIVE_TRANSPORT_ERROR,
   NVKMD_NVGPU_NATIVE_SERVICE_ERROR,
   NVKMD_NVGPU_NATIVE_DRIVER_ERROR,
   NVKMD_NVGPU_NATIVE_INVALID_SIZE,
};

/* Only the selected full-size form is implemented. Zero or short requests
 * and the service's alternate 0xc0104705 header form are not implemented,
 * on purpose. Returns false and writes nothing if arg is NULL or its size is
 * wrong. Give ioctl3 (CMIF 12) a separate full-size Q output descriptor.
 */
bool nvkmd_nvgpu_native_characteristics_init_arg(void *arg, size_t arg_size);

/* Call this only after the transport adapter has validated the IPC
 * envelope, checked that the descriptors are complete, and checked all three
 * result layers for the fixed request above. Sizes are complete readable
 * spans, not allocation capacities. A+0 must report 0xd8. Q is separate from
 * A. Never read the tail of A as Q. Check the results before you touch
 * either buffer, because stale TLS is not a reply. On failure, *out is
 * cleared, so a previous observation cannot stay usable. A NULL out is an
 * invalid argument. Input buffers can be unaligned. DECODED means that the
 * bytes were decoded, also for unknown profiles. It does not mean device
 * support.
 */
enum nvkmd_nvgpu_native_decode_status
nvkmd_nvgpu_native_characteristics_decode(
   struct nvkmd_nvgpu_native_query_results results,
   const void *arg, size_t arg_size, const void *q, size_t q_size,
   struct nvkmd_nvgpu_native_characteristics *out);

enum nvkmd_nvgpu_native_profile_status {
   NVKMD_NVGPU_NATIVE_PROFILE_INVALID,
   NVKMD_NVGPU_NATIVE_PROFILE_UNSUPPORTED,
   NVKMD_NVGPU_NATIVE_PROFILE_UNRESOLVED,
};

/* A first screening of a DECODED observation, not a complete profile
 * validation. UNRESOLVED recognizes only selected scalar and class inputs.
 * These items are still missing: 4706 masks, 4713 topology, 471f engines,
 * the NAK target, and the memory, cache and owner contracts. On purpose,
 * there is no supported or ready result, no nv_device_info conversion and
 * no physical-device publication.
 */
enum nvkmd_nvgpu_native_profile_status
nvkmd_nvgpu_native_profile_screen(
   const struct nvkmd_nvgpu_native_characteristics *chars);

#endif /* NVKMD_NVGPU_NATIVE_PROFILE_H */
