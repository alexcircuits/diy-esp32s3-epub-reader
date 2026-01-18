#ifndef UNIT_TEST
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#define vTaskDelay(t)
#define ESP_LOGE(args...)
#define ESP_LOGI(args...)
#endif

#include <algorithm>

#include "PNGHelper.h"
#include "Renderer.h"

static const char *TAG = "PNG";

#ifdef USE_PNGLE

// PNGLE-based implementation -------------------------------------------------

void pngle_init_callback(pngle_t *pngle, uint32_t w, uint32_t h)
{
  PNGHelper *helper = static_cast<PNGHelper *>(pngle_get_user_data(pngle));
  if (!helper)
  {
    return;
  }
  // compute scaling factors based on destination box
  helper->last_y = -1;
  helper->x_scale = 1.0f;
  helper->y_scale = 1.0f;
  if (w > 0 && h > 0)
  {
    helper->x_scale = static_cast<float>(helper->renderer->get_page_width()) / static_cast<float>(w);
    helper->y_scale = static_cast<float>(helper->renderer->get_page_height()) / static_cast<float>(h);
  }
}

void pngle_draw_callback(pngle_t *pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4])
{
  PNGHelper *helper = static_cast<PNGHelper *>(pngle_get_user_data(pngle));
  if (!helper || !helper->renderer)
  {
    return;
  }

  uint8_t r = rgba[0];
  uint8_t g = rgba[1];
  uint8_t b = rgba[2];
  uint8_t a = rgba[3];

  uint32_t gray = (r * 38 + g * 75 + b * 15) >> 7;
  if (a < 255)
  {
    gray = (gray * a + 255 * (255 - a)) / 255;
  }
  uint8_t gray8 = static_cast<uint8_t>(gray);

  int sx0 = helper->x_pos + static_cast<int>(x * helper->x_scale);
  int sy0 = helper->y_pos + static_cast<int>(y * helper->y_scale);
  int sx1 = helper->x_pos + static_cast<int>((x + w) * helper->x_scale);
  int sy1 = helper->y_pos + static_cast<int>((y + h) * helper->y_scale);

  if (sx1 <= sx0)
  {
    sx1 = sx0 + 1;
  }
  if (sy1 <= sy0)
  {
    sy1 = sy0 + 1;
  }

  for (int yy = sy0; yy < sy1; ++yy)
  {
    if (yy != helper->last_y)
    {
      vTaskDelay(1);
      helper->last_y = yy;
    }
    for (int xx = sx0; xx < sx1; ++xx)
    {
      helper->renderer->draw_pixel(xx, yy, gray8);
    }
  }
}

bool PNGHelper::get_size(const uint8_t *data, size_t data_size, int *width, int *height)
{
  pngle_t *png = pngle_new();
  if (!png)
  {
    ESP_LOGE(TAG, "pngle_new failed");
    return false;
  }

  int local_width = 0;
  int local_height = 0;

  auto init_cb = [](pngle_t *p, uint32_t w, uint32_t h) {
    PNGHelper *self = static_cast<PNGHelper *>(pngle_get_user_data(p));
    if (!self)
    {
      return;
    }
    int *w_ptr = reinterpret_cast<int *>(&self->x_scale); // reuse storage
    int *h_ptr = reinterpret_cast<int *>(&self->y_scale);
    *w_ptr = static_cast<int>(w);
    *h_ptr = static_cast<int>(h);
  };

  pngle_set_user_data(png, this);
  pngle_set_init_callback(png, init_cb);

  int fed = pngle_feed(png, data, data_size);
  if (fed < 0)
  {
    ESP_LOGE(TAG, "pngle error: %s", pngle_error(png));
    pngle_destroy(png);
    return false;
  }

  // Recover width/height from reused storage
  local_width = *reinterpret_cast<int *>(&x_scale);
  local_height = *reinterpret_cast<int *>(&y_scale);

  pngle_destroy(png);

  if (local_width <= 0 || local_height <= 0)
  {
    return false;
  }
  *width = local_width;
  *height = local_height;
  return true;
}

bool PNGHelper::render(const uint8_t *data, size_t data_size, Renderer *renderer, int x_pos, int y_pos, int width, int height)
{
  this->renderer = renderer;
  this->y_pos = y_pos;
  this->x_pos = x_pos;
  this->last_y = -1;
  this->x_scale = 1.0f;
  this->y_scale = 1.0f;

  pngle_t *png = pngle_new();
  if (!png)
  {
    ESP_LOGE(TAG, "pngle_new failed");
    return false;
  }

  pngle_set_user_data(png, this);
  pngle_set_init_callback(png, pngle_init_callback);
  pngle_set_draw_callback(png, pngle_draw_callback);

  int fed = pngle_feed(png, data, data_size);
  if (fed < 0)
  {
    ESP_LOGE(TAG, "pngle error: %s", pngle_error(png));
    pngle_destroy(png);
    return false;
  }

  pngle_destroy(png);
  return true;
}

#else // USE_PNGLE

#include <PNGdec.h>

void convert_rgb_565_to_rgb(uint16_t rgb565, uint8_t *r, uint8_t *g, uint8_t *b)
{
  *r = ((rgb565 >> 11) & 0x1F) << 3;
  *g = ((rgb565 >> 5) & 0x3F) << 2;
  *b = (rgb565 & 0x1F) << 3;
}

bool PNGHelper::get_size(const uint8_t *data, size_t data_size, int *width, int *height)
{
  int rc = png.openRAM(const_cast<uint8_t *>(data), data_size, NULL);
  if (rc == PNG_SUCCESS)
  {
    ESP_LOGI(TAG, "image specs: (%d x %d), %d bpp, pixel type: %d", png.getWidth(), png.getHeight(), png.getBpp(), png.getPixelType());
    *width = png.getWidth();
    *height = png.getHeight();
    return true;
    png.close();
  }
  else
  {
    ESP_LOGE(TAG, "failed to open png %d", rc);
    return false;
  }
}

bool PNGHelper::render(const uint8_t *data, size_t data_size, Renderer *renderer, int x_pos, int y_pos, int width, int height)
{
  this->renderer = renderer;
  this->y_pos = y_pos;
  this->x_pos = x_pos;
  int rc = png.openRAM(const_cast<uint8_t *>(data), data_size, png_draw_callback);
  if (rc == PNG_SUCCESS)
  {
    this->x_scale = float(width) / float(png.getWidth());
    this->y_scale = float(height) / float(png.getHeight());
    this->last_y = -1;
    this->tmp_rgb565_buffer = (uint16_t *)malloc(png.getWidth() * 2);

    png.decode(this, PNG_FAST_PALETTE);
    png.close();
    free(this->tmp_rgb565_buffer);
    return true;
  }
  else
  {
    ESP_LOGE(TAG, "failed to parse png %d", rc);
    return false;
  }
}

void PNGHelper::draw_callback(PNGDRAW *draw)
{
  // work out where we should be drawing this line
  int y = y_pos + draw->y * y_scale;
  // only bother to draw if we haven't already drawn to this destination line
  if (y != last_y)
  {
    // feed the watchdog
    vTaskDelay(1);
    // get the rgb 565 pixel values                 BKG is in form of 00BBGGRR
    png.getLineAsRGB565(draw, tmp_rgb565_buffer, 0, 0x00FFFFFF);
    for (int x = 0; x < png.getWidth() * x_scale; x++)
    {
      uint8_t r, g, b;
      convert_rgb_565_to_rgb(tmp_rgb565_buffer[int(x / x_scale)], &r, &g, &b);
      uint32_t gray = (r * 38 + g * 75 + b * 15) >> 7;
      renderer->draw_pixel(x_pos + x, y, gray);
    }
    last_y = y;
  }
};

void png_draw_callback(PNGDRAW *draw)
{
  PNGHelper *helper = (PNGHelper *)draw->pUser;
  helper->draw_callback(draw);
}

#endif // USE_PNGLE
