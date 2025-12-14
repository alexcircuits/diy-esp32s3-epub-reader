#include "FreeTypeFont.h"

#ifdef USE_FREETYPE

#include "Renderer.h"
#include <string.h>

// Decode the next UTF-8 code point from the string and advance the
// pointer. Returns 0 on error, in which case the caller can skip the
// glyph.
static unsigned int utf8_next_codepoint(const unsigned char *&p)
{
  unsigned char c = *p++;
  // Single-byte (ASCII)
  if (c < 0x80)
  {
    return c;
  }
  // 2-byte sequence
  if ((c & 0xE0) == 0xC0)
  {
    unsigned char c2 = *p;
    if ((c2 & 0xC0) != 0x80)
    {
      return 0;
    }
    ++p;
    return ((c & 0x1F) << 6) | (c2 & 0x3F);
  }
  // 3-byte sequence
  if ((c & 0xF0) == 0xE0)
  {
    unsigned char c2 = p[0];
    unsigned char c3 = p[1];
    if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80)
    {
      return 0;
    }
    p += 2;
    return ((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
  }
  // 4-byte sequence
  if ((c & 0xF8) == 0xF0)
  {
    unsigned char c2 = p[0];
    unsigned char c3 = p[1];
    unsigned char c4 = p[2];
    if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80 || (c4 & 0xC0) != 0x80)
    {
      return 0;
    }
    p += 3;
    return ((c & 0x07) << 18) | ((c2 & 0x3F) << 12) | ((c3 & 0x3F) << 6) | (c4 & 0x3F);
  }
  // Invalid leading byte
  return 0;
}

FreeTypeFont::FreeTypeFont() {}

FreeTypeFont::~FreeTypeFont()
{
  if (m_face)
  {
    FT_Done_Face(m_face);
    m_face = nullptr;
  }
  if (m_library)
  {
    FT_Done_FreeType(m_library);
    m_library = nullptr;
  }
  m_initialized = false;
}

bool FreeTypeFont::init(const char *font_path, int pixel_height)
{
  if (!font_path || pixel_height <= 0)
  {
    return false;
  }

  FT_Error err = FT_Init_FreeType(&m_library);
  if (err != 0)
  {
    return false;
  }

  err = FT_New_Face(m_library, font_path, 0, &m_face);
  if (err != 0)
  {
    FT_Done_FreeType(m_library);
    m_library = nullptr;
    return false;
  }

  // Ensure we use the Unicode charmap if available so that ASCII and
  // UTF-8 text map to sensible glyph indices.
  FT_Select_Charmap(m_face, FT_ENCODING_UNICODE);

  err = FT_Set_Pixel_Sizes(m_face, 0, pixel_height);
  if (err != 0)
  {
    FT_Done_Face(m_face);
    FT_Done_FreeType(m_library);
    m_face = nullptr;
    m_library = nullptr;
    return false;
  }

  m_pixel_height = pixel_height;
  m_initialized = true;
  return true;
}

bool FreeTypeFont::set_pixel_height(int pixel_height)
{
  if (!m_initialized || !m_face || pixel_height <= 0)
  {
    return false;
  }

  FT_Error err = FT_Set_Pixel_Sizes(m_face, 0, pixel_height);
  if (err != 0)
  {
    return false;
  }

  m_pixel_height = pixel_height;
  return true;
}

int FreeTypeFont::get_text_width(const char *text) const
{
  if (!m_initialized || !text)
  {
    return 0;
  }

  int width = 0;
  const unsigned char *p = reinterpret_cast<const unsigned char *>(text);

  while (*p)
  {
    unsigned int codepoint = utf8_next_codepoint(p);
    if (codepoint == 0)
    {
      continue;
    }

    FT_UInt glyph_index = FT_Get_Char_Index(m_face, codepoint);
    FT_Error err = FT_Load_Glyph(m_face, glyph_index, FT_LOAD_DEFAULT);
    if (err == 0)
    {
      // Use the horizontal advance in pixels; if it somehow ends up
      // as zero, fall back to a small reasonable width so that
      // characters do not collapse on top of each other.
      int advance = static_cast<int>(m_face->glyph->advance.x >> 6);
      if (advance <= 0)
      {
        int metrics_advance = static_cast<int>(m_face->glyph->metrics.horiAdvance >> 6);
        if (metrics_advance > 0)
        {
          advance = metrics_advance;
        }
        else
        {
          // Fallback: treat the glyph as at least half a cell wide.
          advance = m_pixel_height > 0 ? m_pixel_height / 2 : 1;
        }
      }
      width += advance;
    }
  }

  return width;
}

int FreeTypeFont::get_line_height() const
{
  if (!m_initialized || !m_face || !m_face->size)
  {
    return m_pixel_height;
  }

  FT_Size_Metrics metrics = m_face->size->metrics;
  if (metrics.height > 0)
  {
    return static_cast<int>(metrics.height >> 6);
  }

  if (metrics.ascender != 0 || metrics.descender != 0)
  {
    // height is typically ascender - descender; both are in 26.6 format
    FT_Pos h = metrics.ascender - metrics.descender;
    if (h > 0)
    {
      return static_cast<int>(h >> 6);
    }
  }

  return m_pixel_height;
}

void FreeTypeFont::draw_text(Renderer *renderer, int x, int y, const char *text) const
{
  if (!m_initialized || !renderer || !text)
  {
    return;
  }

  int pen_x = x;
  // Place baseline roughly at y + pixel_height. This can be refined
  // later using ascender/descender metrics.
  int baseline_y = y + m_pixel_height;

  const unsigned char *p = reinterpret_cast<const unsigned char *>(text);

  while (*p)
  {
    unsigned int codepoint = utf8_next_codepoint(p);
    if (codepoint == 0)
    {
      continue;
    }

    FT_UInt glyph_index = FT_Get_Char_Index(m_face, codepoint);
    FT_Error err = FT_Load_Glyph(m_face, glyph_index, FT_LOAD_DEFAULT);
    if (err != 0)
    {
      ++p;
      continue;
    }

    err = FT_Render_Glyph(m_face->glyph, FT_RENDER_MODE_NORMAL);
    if (err != 0)
    {
      ++p;
      continue;
    }

    FT_GlyphSlot slot = m_face->glyph;
    FT_Bitmap &bmp = slot->bitmap;

    int glyph_x = pen_x + slot->bitmap_left;
    int glyph_y = baseline_y - slot->bitmap_top;

    for (int row = 0; row < static_cast<int>(bmp.rows); ++row)
    {
      const unsigned char *src = bmp.buffer + row * bmp.pitch;
      for (int col = 0; col < static_cast<int>(bmp.width); ++col)
      {
        unsigned char alpha = src[col];
        if (alpha == 0)
        {
          continue;
        }
        // On Paper S3 we favor strong contrast over subtle
        // antialiasing because the grayscale gamma curve tends to
        // wash out light text. Use a simple alpha threshold to draw
        // solid black text.
#if defined(BOARD_TYPE_PAPER_S3)
        if (alpha > 64)
        {
          renderer->draw_pixel(glyph_x + col, glyph_y + row, 0);
        }
#else
        // Default path: map alpha (0 = transparent, 255 = solid) to
        // a grayscale value (0 = black, 255 = white) by inverting it.
        uint8_t gray = static_cast<uint8_t>(255 - alpha);
        renderer->draw_pixel(glyph_x + col, glyph_y + row, gray);
#endif
      }
    }

    int advance = static_cast<int>(slot->advance.x >> 6);
    if (advance <= 0)
    {
      int metrics_advance = static_cast<int>(slot->metrics.horiAdvance >> 6);
      if (metrics_advance > 0)
      {
        advance = metrics_advance;
      }
      else
      {
        int bitmap_width = static_cast<int>(bmp.width);
        if (bitmap_width > 0)
        {
          advance = bitmap_width;
        }
        else
        {
          advance = m_pixel_height > 0 ? m_pixel_height / 2 : 1;
        }
      }
    }
    pen_x += advance;
  }
}

#endif // USE_FREETYPE
