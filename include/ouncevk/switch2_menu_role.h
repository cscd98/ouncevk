/* SPDX-License-Identifier: MIT
 * Private bridge from OunceVK's process-role bootstrap to menu input.
 */
#ifndef OUNCEVK_SWITCH2_MENU_ROLE_H
#define OUNCEVK_SWITCH2_MENU_ROLE_H

#include <stdbool.h>
#include <stdint.h>

/* Call after Vulkan device and swapchain initialization, on the same thread.
 * Native-game input uses resource user ID zero. */
bool ouncevk_switch2_menu_resource_user_id(uint64_t *out);

#endif
