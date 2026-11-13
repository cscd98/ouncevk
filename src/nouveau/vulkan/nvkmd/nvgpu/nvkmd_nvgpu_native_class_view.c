/* SPDX-License-Identifier: MIT */
/* Original OunceVK association of the accepted engine selector with the reduced
 * 3D and compute classes. NVIDIA open-gpu-doc revision 9fdf5c4
 * classes/{3d/clc797.h,compute/clc7c0.h,host/clc56f.h,host/clc76f.h} names
 * the classes.
 */
#include "nvkmd_nvgpu_native_class_view.h"

enum {
   NATIVE_NORMALIZED_GRAPHICS_TYPE = 0,
   NATIVE_NORMALIZED_GRAPHICS_INSTANCE = 0,
   NATIVE_REDUCED_3D_CLASS_C797 = 0xc797,
   NATIVE_REDUCED_COMPUTE_CLASS_C7C0 = 0xc7c0,
   NATIVE_DESCRIPTOR_FIFO_CLASS_C56F = 0xc56f,
   NATIVE_REPORTED_FIFO_CLASS_C76F = 0xc76f,
};

struct nvkmd_nvgpu_native_class_view
nvkmd_nvgpu_native_class_view(const struct nvkmd_nvgpu_native_runtime *r)
{
   const struct nvkmd_nvgpu_native_class_view absent = {0};
   if (r == NULL)
      return absent;

   /* The pure candidate function checks these items: the complete 471f reply
    * from the same owner, the Q copies, the required Q50 gates, the
    * type0/instance0 group membership, and a final selector that is signed
    * non-negative. Do not infer a config value here. Do not read released
    * IPC staging here. */
   const struct nvkmd_nvgpu_native_type0_candidate c =
      nvkmd_nvgpu_native_runtime_type0_candidate(r);
   /* C365/C369 and the required 3D/compute pair are always present. The
    * independent 2D/inline gates can add a maximum of two entries to this
    * fixed array. */
   if (!c.found || c.class_count < 4 ||
       c.class_count > sizeof(c.classes) / sizeof(c.classes[0]) ||
       c.classes[c.class_count - 2] != NATIVE_REDUCED_3D_CLASS_C797 ||
       c.classes[c.class_count - 1] != NATIVE_REDUCED_COMPUTE_CLASS_C7C0)
      return absent;

   /* These are three different observations: descriptor reduction, reported
    * Q classes and normalized group membership. Do not use C56F in place of
    * the reported C76F. Neither class gives permission to operate a
    * channel. */
   const struct nvkmd_nvgpu_native_characteristics *q =
      &r->delegate.characteristics;
   if (q->class_3d != NATIVE_REDUCED_3D_CLASS_C797 ||
       q->class_compute != NATIVE_REDUCED_COMPUTE_CLASS_C7C0 ||
       q->class_gpfifo != NATIVE_REPORTED_FIFO_CLASS_C76F)
      return absent;

   return (struct nvkmd_nvgpu_native_class_view) {
      .associated = true,
      .generation = c.generation,
      .normalized_type = NATIVE_NORMALIZED_GRAPHICS_TYPE,
      .instance = NATIVE_NORMALIZED_GRAPHICS_INSTANCE,
      .selector = c.selector,
      .class_3d = NATIVE_REDUCED_3D_CLASS_C797,
      .class_compute = NATIVE_REDUCED_COMPUTE_CLASS_C7C0,
      .descriptor_fifo_class = NATIVE_DESCRIPTOR_FIFO_CLASS_C56F,
      .reported_fifo_class = q->class_gpfifo,
   };
}
