/*  RetroArch - A frontend for libretro.
 *  Copyright (C) - RetroNX Team
 * 
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdlib.h>

#include <boolean.h>
#include <libretro.h>
#include <retro_miscellaneous.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include "../input_driver.h"

#define MAX_PADS 8

typedef struct nx_input
{
      const input_device_driver_t *joypad;
      bool blocked;
} nx_input_t;

static void nx_input_poll(void *data)
{
      nx_input_t *sw = (nx_input_t *)data;

      if (sw->joypad)
            sw->joypad->poll();
}

static int16_t nx_input_state(void *data,
                                  rarch_joypad_info_t joypad_info,
                                  const struct retro_keybind **binds,
                                  unsigned port, unsigned device,
                                  unsigned idx, unsigned id)
{
      nx_input_t *sw = (nx_input_t *)data;

      if (port > MAX_PADS - 1)
            return 0;

      switch (device)
      {
      case RETRO_DEVICE_JOYPAD:
            return input_joypad_pressed(sw->joypad, joypad_info, port, binds[port], id);
            break;
      case RETRO_DEVICE_ANALOG:
            if (binds[port])
                  return input_joypad_analog(sw->joypad, joypad_info, port, idx, id, binds[port]);
            break;
      }

      return 0;
}

static void nx_input_free_input(void *data)
{
      nx_input_t *sw = (nx_input_t *)data;

      if (sw && sw->joypad)
            sw->joypad->destroy();

      free(sw);
}

static void *nx_input_init(const char *joypad_driver)
{
      nx_input_t *sw = (nx_input_t *)calloc(1, sizeof(*sw));
      if (!sw)
            return NULL;

      sw->joypad = input_joypad_init_driver(joypad_driver, sw);

      return sw;
}

static uint64_t nx_input_get_capabilities(void *data)
{
      (void)data;

      return (1 << RETRO_DEVICE_JOYPAD) | (1 << RETRO_DEVICE_ANALOG);
}

static const input_device_driver_t *nx_input_get_joypad_driver(void *data)
{
      nx_input_t *sw = (nx_input_t *)data;
      if (sw)
            return sw->joypad;
      return NULL;
}

static void nx_input_grab_mouse(void *data, bool state)
{
      (void)data;
      (void)state;
}

static bool nx_input_set_rumble(void *data, unsigned port, enum retro_rumble_effect effect, uint16_t strength)
{
      (void)data;
      (void)port;
      (void)effect;
      (void)strength;

      return false;
}

static bool nx_input_keyboard_mapping_is_blocked(void *data)
{
      nx_input_t *sw = (nx_input_t *)data;
      if (!sw)
            return false;
      return sw->blocked;
}

static void nx_input_keyboard_mapping_set_block(void *data, bool value)
{
      nx_input_t *sw = (nx_input_t *)data;
      if (!sw)
            return;
      sw->blocked = value;
}

input_driver_t input_nx = {
    nx_input_init,
    nx_input_poll,
    nx_input_state,
    nx_input_free_input,
    NULL,
    NULL,
    nx_input_get_capabilities,
    "switch",
    nx_input_grab_mouse,
    NULL,
    nx_input_set_rumble,
    nx_input_get_joypad_driver,
    NULL,
    nx_input_keyboard_mapping_is_blocked,
    nx_input_keyboard_mapping_set_block,
};
