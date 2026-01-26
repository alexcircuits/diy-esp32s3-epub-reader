#pragma once
#include <esp_log.h>

#if defined(BOARD_TYPE_PAPER_S3)
// Paper S3 uses a 4.7" 960x540 panel driven by epdiy; epdiy coordinates are
// 960x540 (width x height) before we apply an inverted portrait rotation.
#include <epdiy.h>
#ifndef EPD_WIDTH
#define EPD_WIDTH 960
#endif
#ifndef EPD_HEIGHT
#define EPD_HEIGHT 540
#endif
#else
#include <epd_driver.h>
#endif

#include <math.h>
#include "Renderer.h"
#include "miniz.h"

#ifdef USE_FREETYPE
#include "FreeTypeFont.h"
#endif

#define GAMMA_VALUE (1.0f / 0.8f)

class EpdiyFrameBufferRenderer : public Renderer
{
protected:
  const EpdFont *m_regular_font;
  const EpdFont *m_bold_font;
  const EpdFont *m_italic_font;
  const EpdFont *m_bold_italic_font;
  const uint8_t *m_busy_image;
  int m_busy_image_width;
  int m_busy_image_height;
  uint8_t *m_frame_buffer;
  EpdFontProperties m_font_props;
  uint8_t gamma_curve[256] = {0};
  bool needs_gray_flush = false;

#ifdef USE_FREETYPE
  FreeTypeFont *m_freetype_font = nullptr;
  bool m_freetype_enabled = false;
#endif

  const EpdFont *get_font(bool is_bold, bool is_italic)
  {
    if (is_bold && is_italic)
    {
      return m_bold_italic_font;
    }
    if (is_bold)
    {
      return m_bold_font;
    }
    if (is_italic)
    {
      return m_italic_font;
    }
    return m_regular_font;
  }

public:
  EpdiyFrameBufferRenderer(
      const EpdFont *regular_font,
      const EpdFont *bold_font,
      const EpdFont *italic_font,
      const EpdFont *bold_italic_font,
      const uint8_t *busy_icon,
      int busy_icon_width,
      int busy_icon_height)
      : m_regular_font(regular_font), m_bold_font(bold_font), m_italic_font(italic_font), m_bold_italic_font(bold_italic_font),
        m_busy_image(busy_icon), m_busy_image_width(busy_icon_width), m_busy_image_height(busy_icon_height)
  {
    m_font_props = epd_font_properties_default();
    // fallback to a question mark for character not available in the font
    m_font_props.fallback_glyph = '?';
    // For Paper S3 we always render in inverted portrait orientation so that
    // logical coordinates are (page_width = 540, page_height = 960).
    epd_set_rotation(EPD_ROT_INVERTED_PORTRAIT);

    for (int gray_value = 0; gray_value < 256; gray_value++)
    {
      gamma_curve[gray_value] = round(255 * pow(gray_value / 255.0, GAMMA_VALUE));
    }
  }
  virtual ~EpdiyFrameBufferRenderer()
  {
  }
  void show_busy()
  {
    int x = (EPD_HEIGHT - m_busy_image_width) / 2;
    int y = (EPD_WIDTH - m_busy_image_height) / 2;
    int width = m_busy_image_width;
    int height = m_busy_image_height;
    EpdRect image_area = {.x = x,
                          .y = y,
                          // don't forget we're rotated...
                          .width = width,
                          .height = height};
    epd_draw_rotated_transparent_image(
        image_area,
        m_busy_image, m_frame_buffer,
        0xE0);
    needs_gray_flush = true;
    flush_area(x, y, width, height);
  }

  void show_img(int x, int y, int width, int height, const uint8_t *img_buffer)
  {
    EpdRect image_area = {.x = x,
                          .y = y,
                          .width = width,
                          .height = height};
    epd_draw_rotated_transparent_image(
        image_area,
        img_buffer, m_frame_buffer,
        0xE0);
  }

  void needs_gray(uint8_t color)
  {
    if (color != 0 && color != 255)
    {
      needs_gray_flush = true;
    }
  }

  bool has_gray() {
    return needs_gray_flush;
  }

  int get_text_width(const char *text, bool bold = false, bool italic = false)
  {
#ifdef USE_FREETYPE
    if (m_freetype_enabled && m_freetype_font && m_freetype_font->is_valid())
    {
      return m_freetype_font->get_text_width(text);
    }
#endif
    if (!m_regular_font)
    {
      // No bitmap font available (e.g. Paper S3 FreeType-only and
      // FreeType failed to initialize). Return a conservative width of
      // zero so callers can handle it gracefully.
      return 0;
    }

    int x = 0, y = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    epd_get_text_bounds(get_font(bold, italic), text, &x, &y, &x1, &y1, &x2, &y2, &m_font_props);
    return x2 - x1;
  }
  void draw_text(int x, int y, const char *text, bool bold = false, bool italic = false)
  {
    // if using antialised text then set to gray next flush
    // needs_gray_flush = true;
#ifdef USE_FREETYPE
    if (m_freetype_enabled && m_freetype_font && m_freetype_font->is_valid())
    {
      int xpos = x + margin_left;
      int ypos = y + margin_top;
      m_freetype_font->draw_text(this, xpos, ypos, text);
      return;
    }
#endif
    if (!m_regular_font)
    {
      // No bitmap font available and FreeType is disabled; nothing to
      // draw.
      return;
    }
    int ypos = y + get_line_height() + margin_top;
    int xpos = x + margin_left;
    epd_write_string(get_font(bold, italic), text, &xpos, &ypos, m_frame_buffer, &m_font_props);
  }
  void draw_rect(int x, int y, int width, int height, uint8_t color = 0)
  {
    needs_gray(color);
    epd_draw_rect({.x = x + margin_left, .y = y + margin_top, .width = width, .height = height}, color, m_frame_buffer);
  }
  virtual void fill_rect(int x, int y, int width, int height, uint8_t color = 0)
  {
    needs_gray(color);
    epd_fill_rect({.x = x + margin_left, .y = y + margin_top, .width = width, .height = height}, color, m_frame_buffer);
  }
  virtual void fill_circle(int x, int y, int r, uint8_t color = 0)
  {
    needs_gray(color);
    epd_fill_circle(x, y, r, color, m_frame_buffer);
  }
  virtual void fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t color)
  {
    needs_gray(color);
    epd_fill_triangle(
        x0 + margin_left, y0 + margin_top,
        x1 + margin_left, y1 + margin_top,
        x2 + margin_left, y2 + margin_top,
        color, m_frame_buffer);
  }
  virtual void draw_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t color)
  {
    needs_gray(color);
    epd_draw_triangle(
        x0 + margin_left, y0 + margin_top,
        x1 + margin_left, y1 + margin_top,
        x2 + margin_left, y2 + margin_top,
        color, m_frame_buffer);
  }
  virtual void draw_pixel(int x, int y, uint8_t color)
  {
    uint8_t corrected_color = gamma_curve[color];
    needs_gray(corrected_color);
    epd_draw_pixel(x + margin_left, y + margin_top, corrected_color, m_frame_buffer);
  }
  virtual void draw_pixels(int x, int y, int width, int height, const uint8_t *data) override
  {
      if (width <= 0 || height <= 0) return;
      
      // Calculate adjusted bounds within the display area
      int start_x = x + margin_left;
      int start_y = y + margin_top;

      // Inverted portrait rotation for Paper S3:
      // Logical (x, y) maps to physical (EPD_WIDTH - 1 - y, x)
      // Since EPD_ROT_INVERTED_PORTRAIT is set, epd_draw_pixel does this transform.
      // We want to write directly to m_frame_buffer (which is physical landscape, EPD_WIDTH x EPD_HEIGHT).
      
      // BUT: Epdiy's m_frame_buffer is 1 byte per pixel. Physical layout is row-major.
      // Physical Resolution: 960x540.
      // Logical Resolution: 540x960.
      
      // The epd_draw_pixel implementation for EPD_ROT_INVERTED_PORTRAIT:
      // _epd_draw_pixel(EPD_WIDTH - 1 - y, x, color, framebuffer);
      // Where EPD_WIDTH = 960 (physical width).
      
      // So for a logical block at (start_x, start_y) of size (width, height):
      // Logical x varies: start_x to start_x + width - 1
      // Logical y varies: start_y to start_y + height - 1
      
      // Physical x = EPD_WIDTH - 1 - logical_y
      // Physical y = logical_x
      
      // This means a logical row (constant logical y, varying logical x) becomes a vertical column in physical memory.
      // A logical column (constant logical x, varying logical y) becomes a horizontal row in physical memory (reversed).
      
      // Optimizing this block transfer is tricky because it's not contiguous in physical memory.
      // However, we can avoid the function call overhead and duplicate gamma lookup.
      
      needs_gray_flush = true; // Assume we are drawing non-white/black pixels
      
      for (int dy = 0; dy < height; ++dy)
      {
          int logical_y = start_y + dy;
          // Pre-calculate physical X for this row
          int phys_x = EPD_WIDTH - 1 - logical_y;
          
          if (phys_x < 0 || phys_x >= EPD_WIDTH) continue;

          for (int dx = 0; dx < width; ++dx)
          {
             int logical_x = start_x + dx;
             int phys_y = logical_x;
             
             if (phys_y < 0 || phys_y >= EPD_HEIGHT) continue;
             
             uint8_t color = data[dy * width + dx];
             
             // Direct framebuffer access
             // Framebuffer is EPD_WIDTH * EPD_HEIGHT bytes
             // Index = phys_y * EPD_WIDTH + phys_x
             m_frame_buffer[phys_y * EPD_WIDTH + phys_x] = gamma_curve[color];
          }
      }
  }
  virtual void draw_circle(int x, int y, int r, uint8_t color = 0)
  {
    needs_gray(color);
    epd_draw_circle(x, y, r, color, m_frame_buffer);
  }
  virtual void flush_display() = 0;
  virtual void flush_area(int x, int y, int width, int height) = 0;

  virtual void clear_screen()
  {
    memset(m_frame_buffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);
  }
  virtual int get_page_width()
  {
    // don't forget we are rotated
    return EPD_HEIGHT - (margin_left + margin_right);
  }
  virtual int get_page_height()
  {
    // don't forget we are rotated
    return EPD_WIDTH - (margin_top + margin_bottom);
  }
  virtual int get_space_width()
  {
    // When using FreeType for the reading view, use the FreeType
    // font's measurement for the width of a space so that layout and
    // rendering stay in sync.
#ifdef USE_FREETYPE
    if (m_freetype_enabled && m_freetype_font && m_freetype_font->is_valid())
    {
      return m_freetype_font->get_text_width(" ");
    }
#endif
    if (!m_regular_font)
    {
      // Fallback when no bitmap font is available.
      return 0;
    }
    auto space_glyph = epd_get_glyph(m_regular_font, ' ');
    return space_glyph->advance_x;
  }
  virtual int get_line_height()
  {
  #ifdef USE_FREETYPE
    if (m_freetype_enabled && m_freetype_font && m_freetype_font->is_valid())
    {
      return m_freetype_font->get_line_height();
    }
  #endif
    if (!m_regular_font)
    {
      return 0;
    }
    return m_regular_font->advance_y;
  }

  // dehydate a frame buffer to file
  virtual bool dehydrate()
  {
    // compress the buffer to save space and increase performance - writing data is slow!
    size_t compressed_size = 0;
    void *compressed = tdefl_compress_mem_to_heap(m_frame_buffer, EPD_WIDTH * EPD_HEIGHT / 2, &compressed_size, 0);
    if (compressed)
    {
      ESP_LOGI("EPD", "Buffer compressed size: %d", compressed_size);
      FILE *fp = fopen("/fs/front_buffer.z", "w");
      if (fp)
      {
        size_t written = fwrite(compressed, 1, compressed_size, fp);
        fclose(fp);
        free(compressed);
        if (written != compressed_size)
        {
          ESP_LOGI("EPD", "Failed to write to file");
          remove("/fs/front_buffer.z");
          return false;
        }
        ESP_LOGI("EPD", "Buffer saved %d", written);
        return true;
      }
    }
    return false;
  }

  // hydrate a frame buffer
  virtual bool hydrate()
  {
    // load the two buffers - the front and the back buffers
    FILE *fp = fopen("/fs/front_buffer.z", "r");
    bool success = false;
    if (fp)
    {
      fseek(fp, 0, SEEK_END);
      size_t compressed_size = ftell(fp);
      fseek(fp, 0, SEEK_SET);
      if (compressed_size > 0)
      {
        ESP_LOGI("EPD", "Buffer compressed size: %d", compressed_size);
        void *compressed = malloc(compressed_size);
        if (compressed)
        {
          fread(compressed, 1, compressed_size, fp);
          int result = tinfl_decompress_mem_to_mem(m_frame_buffer, EPD_WIDTH * EPD_HEIGHT / 2, compressed, compressed_size, 0);
          if (result == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED)
          {
            ESP_LOGE("EPD", "Failed to decompress front buffer");
          }
          else
          {
            success = true;
            ESP_LOGI("EPD", "Success decompressing %d bytes", EPD_WIDTH * EPD_HEIGHT / 2);
          }
          free(compressed);
        }
        else
        {
          ESP_LOGE("EPD", "Failed to allocate memory for front buffer");
        }
      }
      else
      {
        ESP_LOGE("EPD", "No data to restore");
      }
      fclose(fp);
    }
    else
    {
      ESP_LOGI("EPD", "No front buffer found");
    }
    return success;
  }
  virtual void reset() = 0;

#ifdef USE_FREETYPE
  virtual void set_freetype_font_for_reading(FreeTypeFont *font) override
  {
    m_freetype_font = font;
  }
  virtual void set_freetype_enabled(bool enabled) override
  {
    m_freetype_enabled = enabled && (m_freetype_font != nullptr && m_freetype_font->is_valid());
  }

  virtual int get_reading_font_pixel_height() const override
  {
    if (m_freetype_font && m_freetype_font->is_valid())
    {
      return m_freetype_font->get_pixel_height();
    }
    return 0;
  }

  virtual bool set_reading_font_pixel_height(int pixel_height) override
  {
    if (!m_freetype_font || !m_freetype_font->is_valid())
    {
      return false;
    }
    return m_freetype_font->set_pixel_height(pixel_height);
  }
#endif
};