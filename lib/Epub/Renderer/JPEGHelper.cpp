
#ifndef UNIT_TEST
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#if defined(BOARD_HAS_PSRAM)
#include <esp_heap_caps.h>
#endif
#else
#define vTaskDelay(t)
#define ESP_LOGE(args...)
#define ESP_LOGI(args...)
#endif
#include "JPEGHelper.h"
#include "Renderer.h"

static const char *TAG = "JPG";

#define POOL_SIZE 32768

bool JPEGHelper::get_size(const uint8_t *data, size_t data_size, int *width, int *height)
{
  void *pool;
#if !defined(UNIT_TEST) && defined(BOARD_HAS_PSRAM)
  pool = heap_caps_malloc(POOL_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  pool = malloc(POOL_SIZE);
#endif
  if (!pool)
  {
    ESP_LOGE(TAG, "Failed to allocate memory for pool");
    return false;
  }
  m_data = data;
  m_data_pos = 0;
  m_data_size = data_size;
  // decode the jpeg and get its size
  JDEC dec;
  JRESULT res = jd_prepare(&dec, read_jpeg_data, pool, POOL_SIZE, this);
  if (res == JDR_OK)
  {
    ESP_LOGI(TAG, "JPEG Decoded - size %d,%d", dec.width, dec.height);
    *width = dec.width;
    *height = dec.height;
  }
  else
  {
    ESP_LOGE(TAG, "JPEG Decode failed (get_size) - %d", res);
  }
  free(pool);
  m_data = nullptr;
  m_data_pos = 0;
  m_data_size = 0;
  return res == JDR_OK;
}
bool JPEGHelper::render(const uint8_t *data, size_t data_size, Renderer *renderer, int x_pos, int y_pos, int width, int height)
{
  this->renderer = renderer;
  this->y_pos = y_pos;
  this->x_pos = x_pos;
  void *pool;
#if !defined(UNIT_TEST) && defined(BOARD_HAS_PSRAM)
  pool = heap_caps_malloc(POOL_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  pool = malloc(POOL_SIZE);
#endif
  if (!pool)
  {
    ESP_LOGE(TAG, "Failed to allocate memory for pool");
    return false;
  }
  m_data = data;
  m_data_pos = 0;
  m_data_size = data_size;
  // decode the jpeg and get its size
  JDEC dec;
  JRESULT res = jd_prepare(&dec, read_jpeg_data, pool, POOL_SIZE, this);
  if (res == JDR_OK)
  {
    // Correctly calculate the required scale to fit the target dimensions
    float required_x_scale = (float)width / (float)dec.width;
    float required_y_scale = (float)height / (float)dec.height;
    
    // We want the smaller of the two scales to ensure the image fits entirely
    // (aspect fit), or use one if ignoring aspect ratio is intended.
    // Use the smaller scale to fit the image within the bounds.
    float effective_scale = std::min(required_x_scale, required_y_scale);

    // The JPEG decoder only supports downscaling by powers of 2 (1/1, 1/2, 1/4, 1/8)
    // which corresponds to scale_factor 0, 1, 2, 3.
    // calculate the power of 2 that is closest to but not smaller than the effective scale?
    // Actually, we should pick a scale factor such that the decoded image is larger 
    // than the target size, and then we software-scale it down.
    
    scale_factor = 0;
    if (effective_scale <= 0.125f) scale_factor = 3;      // 1/8
    else if (effective_scale <= 0.25f) scale_factor = 2;  // 1/4
    else if (effective_scale <= 0.5f) scale_factor = 1;   // 1/2
    else scale_factor = 0;                                // 1/1

    // Store the actual scaling ratios we will use for the software scaler
    // The decoder will produce an image of size (dec.width >> scale_factor) by (dec.height >> scale_factor)
    int decoded_width = dec.width >> scale_factor;
    int decoded_height = dec.height >> scale_factor;

    // The final software scale needs to map from the decoded size to the target size.
    // effective_scale is relative to the *original* size.
    // We need the scale relative to the *decoded* size.
    this->x_scale = (float)width / (float)decoded_width;
    this->y_scale = (float)height / (float)decoded_height;

    // Use aspect fit: use the smaller scale for both axes to preserve aspect ratio
    // but fill the space as much as possible.
    float final_scale = std::min(this->x_scale, this->y_scale);
    this->x_scale = final_scale;
    this->y_scale = final_scale;

    ESP_LOGI(TAG, "JPEG Decoded - size %d,%d, target %d,%d, scale_factor %d, final scale %f", 
             dec.width, dec.height, width, height, scale_factor, final_scale);
    jd_decomp(&dec, draw_jpeg_function, scale_factor);
  }
  else
  {
    ESP_LOGE(TAG, "JPEG Decode failed (render) - %d", res);
  }
  free(pool);
  m_data = nullptr;
  m_data_pos = 0;
  m_data_size = 0;
  return res == JDR_OK;
}

size_t read_jpeg_data(
    JDEC *jdec,    /* Pointer to the decompression object */
    uint8_t *buff, /* Pointer to buffer to store the read data */
    size_t ndata   /* Number of bytes to read/remove */
)
{
  JPEGHelper *context = (JPEGHelper *)jdec->device;
  if (context->m_data == nullptr)
  {
    ESP_LOGE(TAG, "No image data");
    return 0;
  }
  if (context->m_data_size == 0 || context->m_data_pos >= context->m_data_size)
  {
    ESP_LOGE(TAG, "JPEG input exhausted (pos=%u, size=%u)", (unsigned)context->m_data_pos, (unsigned)context->m_data_size);
    return 0;
  }

  size_t remaining = context->m_data_size - context->m_data_pos;
  size_t to_copy = ndata <= remaining ? ndata : remaining;

  if (buff && to_copy > 0)
  {
    memcpy(buff, context->m_data + context->m_data_pos, to_copy);
  }
  context->m_data_pos += to_copy;
  return to_copy;
}

static int last_y = 0;

// this is not a very efficient way of doing this - could be improved considerably
int draw_jpeg_function(
    JDEC *jdec,   /* Pointer to the decompression object */
    void *bitmap, /* Bitmap to be output */
    JRECT *rect   /* Rectangular region to output */
)
{
  JPEGHelper *context = (JPEGHelper *)jdec->device;
  Renderer *renderer = (Renderer *)context->renderer;
  uint8_t *rgb = (uint8_t *)bitmap;
  // this is a bit of dirty hack to only delay every line to feed the watchdog
  if (rect->top != last_y)
  {
    last_y = rect->top;
    vTaskDelay(1);
  }

  for (int y = rect->top; y <= rect->bottom; y++)
  {
    for (int x = rect->left; x <= rect->right; x++)
    {
      uint8_t r = *rgb++;
      uint8_t g = *rgb++;
      uint8_t b = *rgb++;
      uint32_t gray = (r * 38 + g * 75 + b * 15) >> 7;
      renderer->draw_pixel(context->x_pos + x * context->x_scale, context->y_pos + y * context->y_scale, gray);
    }
  }
  return 1;
}
