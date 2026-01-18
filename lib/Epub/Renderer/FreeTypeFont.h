#pragma once

// Optional FreeType-backed font wrapper used for EPUB reading view.
// All declarations are guarded by USE_FREETYPE so the project can
// still be built without linking against FreeType.

#ifdef USE_FREETYPE

#include <ft2build.h>
#include FT_FREETYPE_H

class Renderer;

class FreeTypeFont
{
public:
  FreeTypeFont();
  ~FreeTypeFont();

  // Initialize the FreeType face from a font file on the filesystem.
  // Returns true on success.
  bool init(const char *font_path, int pixel_height);

  // Measure the advance width of a UTF-8 string in pixels.
  int get_text_width(const char *text) const;

  // Render a UTF-8 string at the given logical coordinates using the
  // supplied Renderer as a pixel sink. The y coordinate should specify
  // the top of the line; the wrapper will place the baseline
  // appropriately based on the font metrics.
  void draw_text(Renderer *renderer, int x, int y, const char *text) const;

  bool is_valid() const { return m_initialized; }

  // Return the recommended line height in pixels based on the current
  // font metrics.
  int get_line_height() const;

  // Return the current requested pixel height that was configured for
  // this face via FT_Set_Pixel_Sizes.
  int get_pixel_height() const { return m_pixel_height; }

  // Update the pixel height for this face at runtime. Returns true on
  // success and leaves the previous size unchanged on failure.
  bool set_pixel_height(int pixel_height);

private:
  FT_Library m_library = nullptr;
  FT_Face m_face = nullptr;
  int m_pixel_height = 0;
  bool m_initialized = false;
};

#endif // USE_FREETYPE
