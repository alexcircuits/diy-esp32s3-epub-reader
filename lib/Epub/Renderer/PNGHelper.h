#pragma once

#include <string>
#include "ImageHelper.h"

class Renderer;

#ifdef USE_PNGLE
#include <pngle.h>
#else
#include <PNGdec.h>
void png_draw_callback(PNGDRAW *draw);
#endif

class PNGHelper : public ImageHelper
{
private:
  // temporary vars used for the PNG callbacks
  Renderer *renderer;
  int x_pos;
  int y_pos;
  int last_y;
  float x_scale;
  float y_scale;
#ifndef USE_PNGLE
  uint16_t *tmp_rgb565_buffer;
  PNG png;

  friend void png_draw_callback(PNGDRAW *draw);
#else
  // Allow pngle callbacks defined in PNGHelper.cpp to access the
  // internal state (renderer, x_pos, y_pos, scales, etc.).
  friend void pngle_init_callback(pngle_t *pngle, uint32_t w, uint32_t h);
  friend void pngle_draw_callback(pngle_t *pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]);
#endif

public:
  bool get_size(const uint8_t *data, size_t data_size, int *width, int *height);
  bool render(const uint8_t *data, size_t data_size, Renderer *renderer, int x_pos, int y_pos, int width, int height);
#ifndef USE_PNGLE
  void draw_callback(PNGDRAW *draw);
#endif
};