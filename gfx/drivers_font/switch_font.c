#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include <encodings/utf.h>

#include <retro_math.h>

#include "../font_driver.h"
#include "../video_driver.h"

#include "../../verbosity.h"

#include "../common/switch_common.h"

typedef struct
{
   switch_texture_t texture;

   const font_renderer_driver_t* font_driver;
   void* font_data;
} switch_font_t;

static void* switch_font_init_font(void* data, const char* font_path,
      float font_size, bool is_threaded)
{
   const struct font_atlas* atlas = NULL;
   switch_font_t* font = (switch_font_t*)calloc(1, sizeof(*font));
   switch_video_t* sw = (switch_video_t*)data;

   if (!font)
      return NULL;

   font_size = 10;
   if (!font_renderer_create_default((const void**)&font->font_driver,
                                     &font->font_data, font_path, font_size))
   {
      RARCH_WARN("Couldn't initialize font renderer.\n");
      free(font);
      return NULL;
   }

   atlas = font->font_driver->get_atlas(font->font_data);

   font->texture.width = next_pow2(atlas->width);
   font->texture.height = next_pow2(atlas->height);

   font->texture.data = malloc(font->texture.width * font->texture.height);
   uint8_t* tmp = font->texture.data;

   int i, j;
   const uint8_t*     src = atlas->buffer;

   //This is where I stopped writing this and went on my journey to rename
   //switch to nx everywhere in the project

   return NULL;
}

static void switch_font_free_font(void* data, bool is_threaded)
{
   (void)data;
}

static int switch_font_get_message_width(void* data, const char* msg,
                                      unsigned msg_len, float scale)
{
   switch_font_t* font = (switch_font_t*)data;

   unsigned i;
   int delta_x = 0;

   if (!font)
      return 0;

   for (i = 0; i < msg_len; i++)
   {
      const char* msg_tmp            = &msg[i];
      unsigned code                  = utf8_walk(&msg_tmp);
      unsigned skip                  = msg_tmp - &msg[i];

      if (skip > 1)
         i += skip - 1;

      const struct font_glyph* glyph =
         font->font_driver->get_glyph(font->font_data, code);

      if (!glyph) /* Do something smarter here ... */
         glyph = font->font_driver->get_glyph(font->font_data, '?');

      if (!glyph)
         continue;

      delta_x += glyph->advance_x;
   }

   return delta_x * scale;
}

static void switch_font_render_line(
      video_frame_info_t *video_info,
      switch_font_t* font, const char* msg, unsigned msg_len,
      float scale, const unsigned int color, float pos_x,
      float pos_y, unsigned text_align)
{
   return;
}

static void switch_font_render_message(
      video_frame_info_t *video_info,
      switch_font_t* font, const char* msg, float scale,
      const unsigned int color, float pos_x, float pos_y,
      unsigned text_align)
{
   int lines = 0;
   float line_height;

   if (!msg || !*msg)
      return;

   /* If the font height is not supported just draw as usual */
   if (!font->font_driver->get_line_height)
   {
      switch_font_render_line(video_info, font, msg, strlen(msg),
                           scale, color, pos_x, pos_y, text_align);
      return;
   }

   line_height = scale / font->font_driver->get_line_height(font->font_data);

   for (;;)
   {
      const char* delim = strchr(msg, '\n');

      /* Draw the line */
      if (delim)
      {
         unsigned msg_len = delim - msg;
         switch_font_render_line(video_info, font, msg, msg_len,
                              scale, color, pos_x, pos_y - (float)lines * line_height,
                              text_align);
         msg += msg_len + 1;
         lines++;
      }
      else
      {
         unsigned msg_len = strlen(msg);
         switch_font_render_line(video_info, font, msg, msg_len,
                              scale, color, pos_x, pos_y - (float)lines * line_height,
                              text_align);
         break;
      }
   }
}

static void switch_font_render_msg(
      video_frame_info_t *video_info,
      void* data, const char* msg,
      const struct font_params *params)
{
   float x, y, scale, drop_mod, drop_alpha;
   int drop_x, drop_y;
   unsigned max_glyphs;
   enum text_alignment text_align;
   unsigned color, color_dark, r, g, b,
            alpha, r_dark, g_dark, b_dark, alpha_dark;
   switch_font_t                * font = (switch_font_t*)data;
   unsigned width                   = video_info->width;
   unsigned height                  = video_info->height;

   if (!font || !msg || !*msg)
      return;

   if (params)
   {
      x                    = params->x;
      y                    = params->y;
      scale                = params->scale;
      text_align           = params->text_align;
      drop_x               = params->drop_x;
      drop_y               = params->drop_y;
      drop_mod             = params->drop_mod;
      drop_alpha           = params->drop_alpha;

      r                    = FONT_COLOR_GET_RED(params->color);
      g                    = FONT_COLOR_GET_GREEN(params->color);
      b                    = FONT_COLOR_GET_BLUE(params->color);
      alpha                = FONT_COLOR_GET_ALPHA(params->color);

      color                = params->color;
   }
   else
   {
      x              = video_info->font_msg_pos_x;
      y              = video_info->font_msg_pos_y;
      scale          = 1.0f;
      text_align     = TEXT_ALIGN_LEFT;

      r              = (video_info->font_msg_color_r * 255);
      g              = (video_info->font_msg_color_g * 255);
      b              = (video_info->font_msg_color_b * 255);
      alpha          = 255;
      color          = COLOR_ABGR(r, g, b, alpha);

      drop_x         = -2;
      drop_y         = -2;
      drop_mod       = 0.3f;
      drop_alpha     = 1.0f;
   }

   max_glyphs        = strlen(msg);

   if (drop_x || drop_y)
      max_glyphs    *= 2;

   if (drop_x || drop_y)
   {
      r_dark         = r * drop_mod;
      g_dark         = g * drop_mod;
      b_dark         = b * drop_mod;
      alpha_dark     = alpha * drop_alpha;
      color_dark     = COLOR_ABGR(r_dark, g_dark, b_dark, alpha_dark);

      switch_font_render_message(video_info, font, msg, scale, color_dark,
                              x + scale * drop_x / width, y +
                              scale * drop_y / height, text_align);
   }

   switch_font_render_message(video_info, font, msg, scale,
                           color, x, y, text_align);
}

static const struct font_glyph* switch_font_get_glyph(
   void* data, uint32_t code)
{
   switch_font_t* font = (switch_font_t*)data;

   if (!font || !font->font_driver)
      return NULL;

   if (!font->font_driver->ident)
      return NULL;

   return font->font_driver->get_glyph((void*)font->font_driver, code);
}

static void switch_font_bind_block(void* data, void* userdata)
{
   (void)data;
}

font_renderer_t switch_font =
{
   switch_font_init_font,
   switch_font_free_font,
   switch_font_render_msg,
   "switchfont",
   switch_font_get_glyph,
   switch_font_bind_block,
   NULL,                         /* flush_block */
   switch_font_get_message_width,
};