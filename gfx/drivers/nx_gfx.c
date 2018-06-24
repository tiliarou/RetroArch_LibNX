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

#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include <retro_inline.h>
#include <retro_math.h>
#include <formats/image.h>

#include "../../libretro-common/include/formats/image.h"
#include <gfx/scaler/scaler.h>
#include <gfx/scaler/pixconv.h>

#include <switch.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#ifdef HAVE_MENU
#include "../../menu/menu_driver.h"
#endif

#include "../font_driver.h"

#include "../../configuration.h"
#include "../../command.h"
#include "../../driver.h"

#include "../../retroarch.h"
#include "../../verbosity.h"

#ifndef HAVE_THREADS
#include "../../tasks/tasks_internal.h"
#endif

// (C) libtransistor
static int pdep(uint32_t mask, uint32_t value)
{
      uint32_t out = 0;
      for (int shift = 0; shift < 32; shift++)
      {
            uint32_t bit = 1u << shift;
            if (mask & bit)
            {
                  if (value & 1)
                        out |= bit;
                  value >>= 1;
            }
      }
      return out;
}

static uint32_t swizzle_x(uint32_t v) { return pdep(~0x7B4u, v); }
static uint32_t swizzle_y(uint32_t v) { return pdep(0x7B4, v); }

void gfx_slow_swizzling_blit(uint32_t *buffer, uint32_t *image, int w, int h, int tx, int ty)
{
      uint32_t *dest = buffer;
      uint32_t *src = image;
      int x0 = tx;
      int y0 = ty;
      int x1 = x0 + w;
      int y1 = y0 + h;
      const uint32_t tile_height = 128;
      const uint32_t padded_width = 128 * 10;

      // we're doing this in pixels - should just shift the swizzles instead
      uint32_t offs_x0 = swizzle_x(x0);
      uint32_t offs_y = swizzle_y(y0);
      uint32_t x_mask = swizzle_x(~0u);
      uint32_t y_mask = swizzle_y(~0u);
      uint32_t incr_y = swizzle_x(padded_width);

      // step offs_x0 to the right row of tiles
      offs_x0 += incr_y * (y0 / tile_height);

      uint32_t x, y;
      for (y = y0; y < y1; y++)
      {
            uint32_t *dest_line = dest + offs_y;
            uint32_t offs_x = offs_x0;

            for (x = x0; x < x1; x++)
            {
                  uint32_t pixel = *src++;
                  dest_line[offs_x] = pixel;
                  offs_x = (offs_x - x_mask) & x_mask;
            }

            offs_y = (offs_y - y_mask) & y_mask;
            if (!offs_y)
                  offs_x0 += incr_y; // wrap into next tile row
      }
}

typedef struct
{
      bool vsync;
      bool rgb32;
      unsigned width, height;
      unsigned rotation;
      struct video_viewport vp;
      struct texture_image *overlay;
      bool overlay_enabled;
      struct
      {
            bool enable;
            bool fullscreen;

            uint32_t *pixels;

            uint32_t width;
            uint32_t height;

            unsigned tgtw;
            unsigned tgth;

            struct scaler_ctx scaler;
      } menu_texture;

      uint32_t image[1280 * 720];
      u32 cnt;
      struct scaler_ctx scaler;
      uint32_t last_width;
      uint32_t last_height;
      bool keep_aspect;
      bool should_resize;

      bool o_size;
      uint32_t o_height;
      uint32_t o_width;
} nx_video_t;

static void *nx_init(const video_info_t *video,
                         const input_driver_t **input, void **input_data)
{
      unsigned x, y;
      void *switchinput = NULL;

      nx_video_t *sw = (nx_video_t *)calloc(1, sizeof(*sw));
      if (!sw)
            return NULL;

      printf("loading switch gfx driver, width: %d, height: %d\n", video->width, video->height);
      sw->vp.x = 0;
      sw->vp.y = 0;
      sw->vp.width = sw->o_width = video->width;
      sw->vp.height = sw->o_height = video->height;
      sw->overlay_enabled = false;
      sw->overlay = NULL;

      sw->vp.full_width = 1280;
      sw->vp.full_height = 720;

      sw->vsync = video->vsync;
      sw->rgb32 = video->rgb32;
      sw->cnt = 0;
      sw->keep_aspect = true;
      sw->should_resize = true;
      sw->o_size = true;

      // Autoselect driver
      if (input && input_data)
      {
            settings_t *settings = config_get_ptr();
            switchinput = input_nx.init(settings->arrays.input_joypad_driver);
            *input = switchinput ? &input_nx : NULL;
            *input_data = switchinput;
      }

      return sw;
}

static void nx_wait_vsync(nx_video_t *sw)
{
      gfxWaitForVsync();
}

static void nx_update_viewport(nx_video_t *sw, video_frame_info_t *video_info)
{
      int x = 0;
      int y = 0;
      float width = sw->vp.full_width;
      float height = sw->vp.full_height;
      if (sw->o_size)
      {
            width = sw->o_width;
            height = sw->o_height;
            sw->vp.x = (int)(((float)sw->vp.full_width - width)) / 2;
            sw->vp.y = (int)(((float)sw->vp.full_height - height)) / 2;

            sw->vp.width = width;
            sw->vp.height = height;

            return;
      }

      settings_t *settings = config_get_ptr();
      float desired_aspect = video_driver_get_aspect_ratio();

      // We crash if >1.0f
      printf("[Video] Aspect: %f\n", desired_aspect);
      /*if (desired_aspect > 1.8f)
            desired_aspect = 1.7778f;

      if (desired_aspect < 1.2f && desired_aspect != 0.0f)
            desired_aspect = 1.0f;*/

      if (settings->bools.video_scale_integer)
      {
            video_viewport_get_scaled_integer(&sw->vp, sw->vp.full_width, sw->vp.full_height, desired_aspect, sw->keep_aspect);
      }
      else if (sw->keep_aspect)
      {
#if defined(HAVE_MENU)
            if (settings->uints.video_aspect_ratio_idx == ASPECT_RATIO_CUSTOM)
            {
                  sw->vp.x = sw->vp.y = 0;
                  sw->vp.width = width;
                  sw->vp.height = height;
            }
            else
#endif
            {
                  float delta;
                  float device_aspect = ((float)sw->vp.full_width) / sw->vp.full_height;

                  if (fabsf(device_aspect - desired_aspect) < 0.0001f)
                  {
                        /* If the aspect ratios of screen and desired aspect
                        * ratio are sufficiently equal (floating point stuff),
                        * assume they are actually equal.
                        */
                  }
                  else if (device_aspect > desired_aspect)
                  {
                        delta = (desired_aspect / device_aspect - 1.0f) / 2.0f + 0.5f;
                        x = (int)roundf(width * (0.5f - delta));
                        width = (unsigned)roundf(2.0f * width * delta);
                  }
                  else
                  {
                        delta = (device_aspect / desired_aspect - 1.0f) / 2.0f + 0.5f;
                        y = (int)roundf(height * (0.5f - delta));
                        height = (unsigned)roundf(2.0f * height * delta);
                  }
            }

            sw->vp.x = x;
            sw->vp.y = y;

            sw->vp.width = width;
            sw->vp.height = height;
      }
      else
      {
            sw->vp.x = sw->vp.y = 0;
            sw->vp.width = width;
            sw->vp.height = height;
      }
}

static void nx_set_aspect_ratio(void *data, unsigned aspect_ratio_idx)
{
      nx_video_t *sw = (nx_video_t *)data;

      if (!sw)
            return;

      sw->keep_aspect = true;
      sw->o_size = false;

      settings_t *settings = config_get_ptr();

      switch (aspect_ratio_idx)
      {
      case ASPECT_RATIO_SQUARE:
            video_driver_set_viewport_square_pixel();
            break;

      case ASPECT_RATIO_CORE:
            video_driver_set_viewport_core();
            sw->o_size = true;
            sw->keep_aspect = false;
            break;

      case ASPECT_RATIO_CONFIG:
            video_driver_set_viewport_config();
            break;

      case ASPECT_RATIO_CUSTOM:
            if (settings->bools.video_scale_integer)
            {
                  video_driver_set_viewport_core();
                  sw->o_size = true;
                  sw->keep_aspect = false;
            }
            break;

      default:
            break;
      }

      video_driver_set_aspect_ratio_value(aspectratio_lut[aspect_ratio_idx].value);

      sw->should_resize = true;
}

static bool nx_frame(void *data, const void *frame,
                         unsigned width, unsigned height,
                         uint64_t frame_count, unsigned pitch,
                         const char *msg, video_frame_info_t *video_info)

{
      if (!appletMainLoop())
            return false;

      static uint64_t last_frame = 0;

      unsigned x, y;
      uint32_t *out_buffer = NULL;
      nx_video_t *sw = data;

      if (sw->should_resize)
      {
            printf("[Video] Requesting new size\n");
            printf("[Video] fw: %i fh: %i w: %i h: %i x: %i y: %i\n", sw->vp.full_width, sw->vp.full_height, sw->vp.width, sw->vp.height, sw->vp.x, sw->vp.y);
            nx_update_viewport(sw, video_info);
            printf("[Video] fw: %i fh: %i w: %i h: %i x: %i y: %i\n", sw->vp.full_width, sw->vp.full_height, sw->vp.width, sw->vp.height, sw->vp.x, sw->vp.y);

            scaler_ctx_gen_reset(&sw->scaler);

            sw->scaler.in_width = width;
            sw->scaler.in_height = height;
            sw->scaler.in_stride = pitch;
            sw->scaler.in_fmt = sw->rgb32 ? SCALER_FMT_ARGB8888 : SCALER_FMT_RGB565;

            sw->scaler.out_width = sw->vp.width;
            sw->scaler.out_height = sw->vp.height;
            sw->scaler.out_stride = sw->vp.full_width * sizeof(uint32_t);
            sw->scaler.out_fmt = SCALER_FMT_ABGR8888;

            sw->scaler.scaler_type = SCALER_TYPE_POINT;

            if (!scaler_ctx_gen_filter(&sw->scaler))
            {
                  printf("failed to generate scaler for main image\n");
                  return false;
            }

            sw->last_width = width;
            sw->last_height = height;

            sw->should_resize = false;
      }

      // Very simple, no overhead (we loop through them anyway!)
      // TODO: memcpy?? duh.
      if (sw->overlay_enabled && sw->overlay != NULL)
      {
            for (y = 0; y < sw->vp.full_height; y++)
            {
                  for (x = 0; x < sw->vp.full_width; x++)
                  {
                        sw->image[y * sw->vp.full_width + x] = sw->overlay->pixels[y * sw->vp.full_width + x];
                  }
            }
      }
      else
      {
            // uint32_t image[1280 * 720];
            memset(&sw->image, 0, sizeof(sw->image));
      }

      if (width > 0 && height > 0)
      {
            scaler_ctx_scale(&sw->scaler, sw->image + (sw->vp.y * sw->vp.full_width) + sw->vp.x, frame);
      }

      if (sw->menu_texture.enable)
      {
            menu_driver_frame(video_info);

            if (sw->menu_texture.pixels)
            {
                  scaler_ctx_scale(&sw->menu_texture.scaler, sw->image + ((sw->vp.full_height - sw->menu_texture.tgth) / 2) * sw->vp.full_width + ((sw->vp.full_width - sw->menu_texture.tgtw) / 2), sw->menu_texture.pixels);
            }
      }
      else if (video_info->statistics_show)
      {
            struct font_params *osd_params = (struct font_params *)&video_info->osd_stat_params;

            if (osd_params)
            {
                  font_driver_render_msg(video_info, NULL, video_info->stat_text,
                                         (const struct font_params *)&video_info->osd_stat_params);
            }
      }

      //if (msg && strlen(msg) > 0)
      //      printf("message: %s\n", msg);

      width = 0;
      height = 0;

      out_buffer = (uint32_t *)gfxGetFramebuffer(&width, &height);
      if (sw->cnt == 60)
      {
            sw->cnt = 0;
      }
      else
      {
            sw->cnt++;
      }

      gfx_slow_swizzling_blit(out_buffer, sw->image, sw->vp.full_width, sw->vp.full_height, 0, 0);
      gfxFlushBuffers();
      gfxSwapBuffers();
      if (sw->vsync)
            nx_wait_vsync(sw);

      last_frame = svcGetSystemTick();

      return true;
}

static void nx_set_nonblock_state(void *data, bool toggle)
{
      nx_video_t *sw = data;
      sw->vsync = !toggle;
}

static bool nx_alive(void *data)
{
      (void)data;
      return true;
}

static bool nx_focus(void *data)
{
      (void)data;
      return true;
}

static bool nx_suppress_screensaver(void *data, bool enable)
{
      (void)data;
      (void)enable;
      return false;
}

static bool nx_has_windowed(void *data)
{
      (void)data;
      return false;
}

static void nx_free(void *data)
{
      nx_video_t *sw = data;
      if (sw->menu_texture.pixels)
            free(sw->menu_texture.pixels);

      free(sw);
}

static bool nx_set_shader(void *data,
                              enum rarch_shader_type type, const char *path)
{
      (void)data;
      (void)type;
      (void)path;

      return false;
}

static void nx_set_rotation(void *data, unsigned rotation)
{
      nx_video_t *sw = data;
      if (!sw)
            return;
      sw->rotation = rotation;
}

static void nx_viewport_info(void *data, struct video_viewport *vp)
{
      nx_video_t *sw = data;
      *vp = sw->vp;
}

static bool nx_read_viewport(void *data, uint8_t *buffer, bool is_idle)
{
      (void)data;
      (void)buffer;

      return true;
}

static void nx_set_texture_frame(
    void *data, const void *frame, bool rgb32,
    unsigned width, unsigned height, float alpha)
{

      nx_video_t *sw = data;

      if (!sw->menu_texture.pixels ||
          sw->menu_texture.width != width ||
          sw->menu_texture.height != height)
      {
            if (sw->menu_texture.pixels)
                  free(sw->menu_texture.pixels);

            sw->menu_texture.pixels = malloc(width * height * (rgb32 ? 4 : 2));
            if (!sw->menu_texture.pixels)
            {
                  printf("failed to allocate buffer for menu texture\n");
                  return;
            }

            int xsf = 1280 / width;
            int ysf = 720 / height;
            int sf = xsf;

            if (ysf < sf)
                  sf = ysf;

            sw->menu_texture.width = width;
            sw->menu_texture.height = height;
            sw->menu_texture.tgtw = width * sf;
            sw->menu_texture.tgth = height * sf;

            struct scaler_ctx *sctx = &sw->menu_texture.scaler;
            scaler_ctx_gen_reset(sctx);

            sctx->in_width = width;
            sctx->in_height = height;
            sctx->in_stride = width * (rgb32 ? 4 : 2);
            sctx->in_fmt = rgb32 ? SCALER_FMT_ARGB8888 : SCALER_FMT_RGB565;

            sctx->out_width = sw->menu_texture.tgtw;
            sctx->out_height = sw->menu_texture.tgth;
            sctx->out_stride = 1280 * 4;
            sctx->out_fmt = SCALER_FMT_ABGR8888;

            sctx->scaler_type = SCALER_TYPE_POINT;

            if (!scaler_ctx_gen_filter(sctx))
            {
                  printf("failed to generate scaler for menu texture\n");
                  return;
            }
      }

      memcpy(sw->menu_texture.pixels, frame, width * height * (rgb32 ? 4 : 2));
}

static void nx_apply_state_changes(void *data)
{
      nx_video_t *sw = (nx_video_t *)data;
}

static void nx_set_texture_enable(void *data, bool enable, bool full_screen)
{
      nx_video_t *sw = data;
      sw->menu_texture.enable = enable;
      sw->menu_texture.fullscreen = full_screen;
}

#ifdef HAVE_OVERLAY
static void nx_overlay_enable(void *data, bool state)
{
      printf("[Video] Enabled Overlay\n");

      nx_video_t *swa = (nx_video_t *)data;

      if (!swa)
            return;

      swa->overlay_enabled = state;
}

static bool nx_overlay_load(void *data,
                                const void *image_data, unsigned num_images)
{
      nx_video_t *swa = (nx_video_t *)data;

      struct texture_image *images = (struct texture_image *)image_data;

      if (!swa)
            return false;

      swa->overlay = images;
      swa->overlay_enabled = true;

      return true;
}

static void nx_overlay_tex_geom(void *data,
                                    unsigned idx, float x, float y, float w, float h)
{
      nx_video_t *swa = (nx_video_t *)data;

      if (!swa)
            return;
}

static void nx_overlay_vertex_geom(void *data,
                                       unsigned idx, float x, float y, float w, float h)
{
      nx_video_t *swa = (nx_video_t *)data;

      if (!swa)
            return;
}

static void nx_overlay_full_screen(void *data, bool enable)
{
      nx_video_t *swa = (nx_video_t *)data;
}

static void nx_overlay_set_alpha(void *data, unsigned idx, float mod)
{
      nx_video_t *swa = (nx_video_t *)data;

      if (!swa)
            return;
}

static const video_overlay_interface_t nx_overlay = {
    nx_overlay_enable,
    nx_overlay_load,
    nx_overlay_tex_geom,
    nx_overlay_vertex_geom,
    nx_overlay_full_screen,
    nx_overlay_set_alpha,
};

void nx_overlay_interface(void *data, const video_overlay_interface_t **iface)
{
      nx_video_t *swa = (nx_video_t *)data;
      if (!swa)
            return;
      *iface = &nx_overlay;
}

#endif

static const video_poke_interface_t nx_poke_interface = {
    NULL,                       /* get_flags */
    NULL,                       /* set_coords */
    NULL,                       /* set_mvp */
    NULL,                       /* load_texture */
    NULL,                       /* unload_texture */
    NULL,                       /* set_video_mode */
    NULL,                       /* get_refresh_rate */
    NULL,                       /* set_filtering */
    NULL,                       /* get_video_output_size */
    NULL,                       /* get_video_output_prev */
    NULL,                       /* get_video_output_next */
    NULL,                       /* get_current_framebuffer */
    NULL,                       /* get_proc_address */
    nx_set_aspect_ratio,    /* set_aspect_ratio */
    nx_apply_state_changes, /* apply_state_changes */
    nx_set_texture_frame,
    nx_set_texture_enable,
    NULL, /* set_osd_msg */
    NULL, /* show_mouse */
    NULL, /* grab_mouse_toggle */
    NULL, /* get_current_shader */
    NULL, /* get_current_software_framebuffer */
    NULL, /* get_hw_render_interface */
};

static void nx_get_poke_interface(void *data,
                                      const video_poke_interface_t **iface)
{
      (void)data;
      *iface = &nx_poke_interface;
}

video_driver_t video_nx = {
    nx_init,
    nx_frame,
    nx_set_nonblock_state,
    nx_alive,
    nx_focus,
    nx_suppress_screensaver,
    nx_has_windowed,
    nx_set_shader,
    nx_free,
    "switch",
    NULL, /* set_viewport */
    nx_set_rotation,
    nx_viewport_info,
    nx_read_viewport,
    NULL, /* read_frame_raw */
#ifdef HAVE_OVERLAY
    nx_overlay_interface, /* nx_overlay_interface */
#endif
    nx_get_poke_interface,
};
