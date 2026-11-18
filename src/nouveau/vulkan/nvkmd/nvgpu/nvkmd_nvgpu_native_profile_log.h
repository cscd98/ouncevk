/* SPDX-License-Identifier: MIT
 * The driver's profile log: when sdmc:/config/ouncevk/profile-log.txt
 * exists, a thread writes one line per interval to
 * sdmc:/config/ouncevk/profile.log, for any program. See
 * <ouncevk/profile.h>.
 */
#ifndef NVKMD_NVGPU_NATIVE_PROFILE_LOG_H
#define NVKMD_NVGPU_NATIVE_PROFILE_LOG_H

struct nvkmd_nvgpu_native_device_owner;

/* Starts the log for the device, when its file asks for it. Call after the
 * device opened. Does nothing when the log already runs. */
void nvkmd_nvgpu_native_profile_log_start(
   struct nvkmd_nvgpu_native_device_owner *owner);

/* Writes the last line and stops the log. Call before the device closes. */
void nvkmd_nvgpu_native_profile_log_stop(void);

#endif
