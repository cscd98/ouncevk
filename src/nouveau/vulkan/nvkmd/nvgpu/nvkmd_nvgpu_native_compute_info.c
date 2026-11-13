/* SPDX-License-Identifier: MIT */
/* Experimental source projection of the selected native model 0x808. The
 * class, topology and scheduling values are observations. Our NAK shader
 * tested only one 24-GPR, zero-shared instruction subset.
 */
#include "nvkmd_nvgpu_native_compute_info.h"

#include <string.h>

enum {
   /* The ISA selection is an experimental NAK encoding choice. The device
    * does not report it, and it does not assert full SM86 compatibility. */
   NATIVE_EXPERIMENTAL_NAK_SM = 86,
   NATIVE_SELECTED_CHIPSET_CANDIDATE = 0x17e,
   NATIVE_SELECTED_MP_PER_TPC_CANDIDATE = 2,
   NATIVE_SELECTED_M2MF_CLASS = 0xa140,
};

enum nvkmd_nvgpu_native_compute_info_status
nvkmd_nvgpu_native_compute_info(
   const struct nvkmd_nvgpu_native_characteristics *chars,
   const struct nvkmd_nvgpu_native_resource_policy *policy,
   bool allow_experimental_sm86,
   struct nv_device_info *out)
{
   if (out == NULL)
      return NVKMD_NVGPU_COMPUTE_INFO_ARGUMENT;
   memset(out, 0, sizeof(*out));
   if (chars == NULL || policy == NULL)
      return NVKMD_NVGPU_COMPUTE_INFO_ARGUMENT;
   if (!allow_experimental_sm86)
      return NVKMD_NVGPU_COMPUTE_INFO_NOT_ENABLED;
   if (nvkmd_nvgpu_native_profile_screen(chars) !=
       NVKMD_NVGPU_NATIVE_PROFILE_UNRESOLVED)
      return NVKMD_NVGPU_COMPUTE_INFO_PROFILE;

   const __typeof__(policy->scheduling) *scheduling = &policy->scheduling;
   if (scheduling->validity != NVKMD_NVGPU_NATIVE_POLICY_STATIC_CANDIDATE ||
       scheduling->compute_class != chars->class_compute ||
       scheduling->carveout_count != NVKMD_NVGPU_NATIVE_C7C0_CARVEOUT_COUNT ||
       scheduling->threads_per_warp != 32 ||
       scheduling->resident_warps != 48 ||
       scheduling->shared_rounding_B != 256 ||
       policy->spa_version.validity != NVKMD_NVGPU_NATIVE_POLICY_STATIC_CANDIDATE ||
       policy->spa_version.value != 0x806)
      return NVKMD_NVGPU_COMPUTE_INFO_POLICY;
   static const uint32_t expected_carveouts_B[] = {
      0, 8192, 16384, 32768, 65536, 102400,
   };
   if (memcmp(scheduling->carveouts_B, expected_carveouts_B,
              sizeof(expected_carveouts_B)) != 0)
      return NVKMD_NVGPU_COMPUTE_INFO_POLICY;

   /* Keep the native model and the compiler policy separate. The selected
    * descriptor reports two MPs per TPC. This value is a candidate for model
    * 808 only, not a universal topology formula. */
   out->type = NV_DEVICE_TYPE_SOC;
   out->chipset = NATIVE_SELECTED_CHIPSET_CANDIDATE;
   memcpy(out->device_name, "Switch 2 GPU", sizeof("Switch 2 GPU"));
   memcpy(out->chipset_name, "model 0x808", sizeof("model 0x808"));
   out->sm = NATIVE_EXPERIMENTAL_NAK_SM;
   out->gpc_count = (uint8_t)chars->gpc_count;
   out->tpc_count = (uint16_t)(chars->gpc_count * chars->tpc_per_gpc);
   out->mp_per_tpc = NATIVE_SELECTED_MP_PER_TPC_CANDIDATE;
   out->max_warps_per_mp = (uint8_t)scheduling->resident_warps;
   /* The non-coherent atom is our flush rounding, not a hardware value. The
    * native map maintenance is a full-system barrier (dsb sy), so any atom
    * is correct. Vulkan allows at most 256; use the 64-byte CPU cache line.
    */
   out->nc_atom_size_B = 64;
   out->cls_copy = (uint16_t)chars->class_copy;
   out->cls_eng2d = (uint16_t)chars->class_2d;
   out->cls_eng3d = (uint16_t)chars->class_3d;
   out->cls_m2mf = NATIVE_SELECTED_M2MF_CLASS;
   out->cls_compute = (uint16_t)chars->class_compute;
   out->cls_gpfifo = (uint16_t)chars->class_gpfifo;
   out->sm_smem_size_count = (uint8_t)scheduling->carveout_count;
   for (uint32_t i = 0; i < scheduling->carveout_count; i++)
      out->sm_smem_sizes_kB[i] = (uint16_t)(scheduling->carveouts_B[i] / 1024);
   /* Per-workgroup shared memory. NVK advertises maxComputeSharedMemorySize
    * = MIN2(NVK_MAX_SHARED_SIZE (48 KiB), largest carveout). NAK's QMD fill
    * asserts each shader's size against this field (qmd.rs). Thus a zero
    * value here makes every shared-memory dispatch abort. 48 KiB is that
    * advertised limit, within the native C7C0 carveout table (100 KiB per
    * SM). The self-test's workgroup test verified it up to 48 KiB. A larger
    * per-workgroup maximum is not established.
    */
   out->max_smem_per_wg_kB = 48;

   /* Device ID and memory budget stay zero. Do not use these values in the
    * ordinary feature and property path until those fields are validated
    * separately on the console and accepted by the API. */
   return NVKMD_NVGPU_COMPUTE_INFO_CANDIDATE;
}
