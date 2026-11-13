/* SPDX-License-Identifier: MIT */

/* OunceVK's decoding of the GPU characteristics and the resource policy. A
 * recognized profile does not by itself show native-role acquisition, a
 * compiler target, or Vulkan device support.
 */

#include "nvkmd_nvgpu_native_profile.h"

#include <string.h>

static uint32_t
read_le32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t
read_le64(const uint8_t *p)
{
   return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

static int16_t
read_le_i16(const uint8_t *p)
{
   const int32_t u = p[0] | ((int32_t)p[1] << 8);
   return (int16_t)(u >= 0x8000 ? u - 0x10000 : u);
}

bool
nvkmd_nvgpu_native_characteristics_init_arg(void *arg, size_t arg_size)
{
   if (arg == NULL || arg_size != NVKMD_NVGPU_NATIVE_CHARACTERISTICS_ARG_SIZE)
      return false;

   uint8_t *a = arg;
   memset(a, 0, arg_size);
   a[0] = NVKMD_NVGPU_NATIVE_CHARACTERISTICS_SIZE;
   /* A+8 is zero, not a CPU pointer. The output uses a separate descriptor. */
   return true;
}

enum nvkmd_nvgpu_native_decode_status
nvkmd_nvgpu_native_characteristics_decode(
   struct nvkmd_nvgpu_native_query_results results,
   const void *arg, size_t arg_size, const void *q, size_t q_size,
   struct nvkmd_nvgpu_native_characteristics *out)
{
   if (out == NULL)
      return NVKMD_NVGPU_NATIVE_INVALID_ARGUMENT;

   /* Decode into a temporary value. Thus all input is read before out is
    * written, even when the buffers overlap. Every failure clears the
    * previous output observation.
    */
   struct nvkmd_nvgpu_native_characteristics decoded;
   memset(&decoded, 0, sizeof(decoded));
   enum nvkmd_nvgpu_native_decode_status status;
   if (results.transport != 0) {
      status = NVKMD_NVGPU_NATIVE_TRANSPORT_ERROR;
   } else if (results.service != 0) {
      status = NVKMD_NVGPU_NATIVE_SERVICE_ERROR;
   } else if (results.driver != 0) {
      status = NVKMD_NVGPU_NATIVE_DRIVER_ERROR;
   } else if (arg == NULL || q == NULL) {
      status = NVKMD_NVGPU_NATIVE_INVALID_ARGUMENT;
   } else if (arg_size != NVKMD_NVGPU_NATIVE_CHARACTERISTICS_ARG_SIZE ||
              q_size != NVKMD_NVGPU_NATIVE_CHARACTERISTICS_SIZE) {
      status = NVKMD_NVGPU_NATIVE_INVALID_SIZE;
   } else if (read_le64(arg) != NVKMD_NVGPU_NATIVE_CHARACTERISTICS_SIZE) {
      status = NVKMD_NVGPU_NATIVE_INVALID_SIZE;
   } else {
      const uint8_t *p = q;
      memcpy(decoded.raw, p, sizeof(decoded.raw));
      decoded.arch = read_le32(p + 0x00);
      decoded.impl = read_le32(p + 0x04);
      decoded.revision = read_le32(p + 0x08);
      decoded.gpc_count = read_le32(p + 0x0c);
      decoded.gpu_l2_size_B = read_le64(p + 0x10);
      decoded.memory_raw = read_le64(p + 0x18);
      decoded.tpc_per_gpc = read_le32(p + 0x20);
      decoded.q24_raw = read_le32(p + 0x24);
      decoded.big_page_size_raw = read_le32(p + 0x28);
      decoded.compression_page_size_raw = read_le32(p + 0x2c);
      decoded.pde_coverage_raw = read_le32(p + 0x30);
      decoded.big_page_mask_raw = read_le32(p + 0x34);
      decoded.gpc_mask = read_le32(p + 0x38);
      decoded.native_version_a = read_le32(p + 0x3c);
      decoded.native_version_b = read_le32(p + 0x40);
      decoded.warp_count_raw = read_le32(p + 0x44);
      decoded.gpu_va_bits = p[0x48];
      decoded.flags = read_le64(p + 0x50);
      decoded.class_2d = read_le32(p + 0x58);
      decoded.class_3d = read_le32(p + 0x5c);
      decoded.class_compute = read_le32(p + 0x60);
      decoded.class_gpfifo = read_le32(p + 0x64);
      decoded.class_inline = read_le32(p + 0x68);
      decoded.class_copy = read_le32(p + 0x6c);
      for (unsigned i = 0; i < 8; i++)
         decoded.versions[i] = read_le_i16(p + 0x70 + 2 * i);
      decoded.gpc_count_copy = read_le32(p + 0x98);
      status = NVKMD_NVGPU_NATIVE_DECODED;
   }
   memcpy(out, &decoded, sizeof(decoded));
   return status;
}

/* Selected characteristics Q-field values. They come from one console and
 * are not universal hardware limits. NVIDIA open-gpu-doc
 * 9fdf5c4 names the C797/C7C0/C7B5/C76F classes in
 * classes/{3d/clc797.h,compute/clc7c0.h,dma-copy/clc7b5.h,host/clc76f.h}.
 * No header definition is copied. Q40 raw 806 and the SPA packet 806 are
 * different fields, although their values are equal.
 */
enum {
   NATIVE_SELECTED_Q00_ARCH = 0x170,
   NATIVE_SELECTED_Q04_IMPL = 0xe,
   NATIVE_REVISION_EXCLUSIVE_LIMIT = 0x100,
   NATIVE_SELECTED_Q08_REVISION = 0xa1,
   NATIVE_SELECTED_Q0C_GPC_COUNT = 1,
   NATIVE_SELECTED_Q20_TPC_PER_GPC = 6,
   NATIVE_SELECTED_Q38_GPC_MASK = 1,
   NATIVE_SELECTED_Q3C_MODEL = 0x808,
   NATIVE_SELECTED_Q40_VERSION_B = 0x806,
   NATIVE_SELECTED_Q44_WARP_RAW = 0x60,
   NATIVE_SELECTED_Q48_GPU_VA_BITS = 40,
   NATIVE_SELECTED_Q58_CLASS_2D = 0x902d,
   NATIVE_SELECTED_Q5C_CLASS_3D = 0xc797,
   NATIVE_C7C0_COMPUTE_CLASS = 0xc7c0,
   NATIVE_SELECTED_Q64_CLASS_GPFIFO = 0xc76f,
   NATIVE_SELECTED_Q68_CLASS_INLINE = 0xa140,
   NATIVE_SELECTED_Q6C_CLASS_COPY = 0xc7b5,
};

enum nvkmd_nvgpu_native_profile_status
nvkmd_nvgpu_native_profile_screen(
   const struct nvkmd_nvgpu_native_characteristics *chars)
{
   if (chars == NULL)
      return NVKMD_NVGPU_NATIVE_PROFILE_INVALID;

   /* Compare each field separately. ORing or masking unknown inputs can
    * produce a false 17e. */
   if (chars->arch != NATIVE_SELECTED_Q00_ARCH ||
       chars->impl != NATIVE_SELECTED_Q04_IMPL)
      return NVKMD_NVGPU_NATIVE_PROFILE_UNSUPPORTED;

   uint32_t enabled_gpcs = 0;
   for (uint32_t mask = chars->gpc_mask; mask != 0; mask &= mask - 1)
      enabled_gpcs++;

   if (chars->revision >= NATIVE_REVISION_EXCLUSIVE_LIMIT ||
       chars->gpc_count == 0 ||
       chars->gpc_count != chars->gpc_count_copy ||
       chars->gpc_count != enabled_gpcs || chars->tpc_per_gpc == 0)
      return NVKMD_NVGPU_NATIVE_PROFILE_INVALID;

   /* Narrow recognition of saved scalar/class inputs, not a universal 17e
    * profile. Conditional class suppression and other revisions need their
    * own validation. Do not normalize C76F to C56F.
    */
   if (chars->revision != NATIVE_SELECTED_Q08_REVISION ||
       chars->gpc_count != NATIVE_SELECTED_Q0C_GPC_COUNT ||
       chars->gpc_mask != NATIVE_SELECTED_Q38_GPC_MASK ||
       chars->tpc_per_gpc != NATIVE_SELECTED_Q20_TPC_PER_GPC ||
       chars->native_version_a != NATIVE_SELECTED_Q3C_MODEL ||
       chars->native_version_b != NATIVE_SELECTED_Q40_VERSION_B ||
       chars->warp_count_raw != NATIVE_SELECTED_Q44_WARP_RAW ||
       chars->gpu_va_bits != NATIVE_SELECTED_Q48_GPU_VA_BITS ||
       chars->class_2d != NATIVE_SELECTED_Q58_CLASS_2D ||
       chars->class_3d != NATIVE_SELECTED_Q5C_CLASS_3D ||
       chars->class_compute != NATIVE_C7C0_COMPUTE_CLASS ||
       chars->class_gpfifo != NATIVE_SELECTED_Q64_CLASS_GPFIFO ||
       chars->class_inline != NATIVE_SELECTED_Q68_CLASS_INLINE ||
       chars->class_copy != NATIVE_SELECTED_Q6C_CLASS_COPY)
      return NVKMD_NVGPU_NATIVE_PROFILE_UNSUPPORTED;

   /* Even the observed values have no confirmed native NAK, cache, memory or
    * engine contract. Never fall back to the GM20B or Mesa defaults.
    */
   return NVKMD_NVGPU_NATIVE_PROFILE_UNRESOLVED;
}

/* Model 808 selects SPA packet value 806 for C7C0 compute and C797 3D. Q40's
 * numerically equal raw 806 is a separate field and does not drive this
 * packet. NVIDIA open-gpu-doc 9fdf5c4
 * classes/{compute/clc7c0.h,3d/clc797.h,host/clc76f.h} confirms
 * SET_SPA_VERSION method 0x310 and the packet fields. No code or header
 * definitions from it are copied here. This does not resolve the shader ISA.
 */
enum {
   NATIVE_MODEL_808_SPA_PACKET_VERSION = 0x806,
   NATIVE_C7C0_SHARED_ROUNDING_B = 256,
   NATIVE_C7C0_THREADS_PER_WARP = 32,
   NATIVE_C7C0_RESIDENT_WARPS = 48,
};

void
nvkmd_nvgpu_native_resource_policy_decode(
   const struct nvkmd_nvgpu_native_characteristics *chars,
   struct nvkmd_nvgpu_native_resource_policy *out)
{
   if (out == NULL)
      return;

   struct nvkmd_nvgpu_native_resource_policy policy;
   memset(&policy, 0, sizeof(policy));
   if (chars != NULL) {
      policy.q3c = (struct nvkmd_nvgpu_native_policy_scalar) {
         chars->native_version_a, NVKMD_NVGPU_NATIVE_POLICY_RAW};
      policy.q40 = (struct nvkmd_nvgpu_native_policy_scalar) {
         chars->native_version_b, NVKMD_NVGPU_NATIVE_POLICY_RAW};
      policy.q44 = (struct nvkmd_nvgpu_native_policy_scalar) {
         chars->warp_count_raw, NVKMD_NVGPU_NATIVE_POLICY_RAW};
      policy.compute_class_raw = (struct nvkmd_nvgpu_native_policy_scalar) {
         chars->class_compute, NVKMD_NVGPU_NATIVE_POLICY_RAW};
      if (nvkmd_nvgpu_native_profile_screen(chars) ==
          NVKMD_NVGPU_NATIVE_PROFILE_UNRESOLVED) {
         /* Static association only: Q60 is not the selected class.
          * Do not implement occupancy, capped allocation search or QMD writes.
          */
         const uint32_t carveouts_B[] = {0, 8192, 16384, 32768, 65536, 102400};
         _Static_assert(sizeof(carveouts_B) == sizeof(policy.scheduling.carveouts_B),
                        "C7C0 carveout count");
         policy.scheduling.validity = NVKMD_NVGPU_NATIVE_POLICY_STATIC_CANDIDATE;
         policy.scheduling.compute_class = NATIVE_C7C0_COMPUTE_CLASS;
         policy.scheduling.carveout_count = NVKMD_NVGPU_NATIVE_C7C0_CARVEOUT_COUNT;
         memcpy(policy.scheduling.carveouts_B, carveouts_B, sizeof(carveouts_B));
         policy.scheduling.shared_rounding_B = NATIVE_C7C0_SHARED_ROUNDING_B;
         policy.scheduling.threads_per_warp = NATIVE_C7C0_THREADS_PER_WARP;
         policy.scheduling.resident_warps = NATIVE_C7C0_RESIDENT_WARPS;
         policy.spa_version = (struct nvkmd_nvgpu_native_policy_scalar) {
            NATIVE_MODEL_808_SPA_PACKET_VERSION,
            NVKMD_NVGPU_NATIVE_POLICY_STATIC_CANDIDATE,
         };
      }
   }
   memcpy(out, &policy, sizeof(policy));
}
