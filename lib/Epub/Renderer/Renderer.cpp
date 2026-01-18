#include "Renderer.h"
#include "JPEGHelper.h"
#include "PNGHelper.h"
#ifndef UNIT_TEST
#include <esp_log.h>
#else
#define vTaskDelay(t)
#define ESP_LOGE(args...)
#define ESP_LOGI(args...)
#endif

Renderer::~Renderer()
{
  delete png_helper;
  delete jpeg_helper;
}

ImageHelper *Renderer::get_image_helper(const std::string &filename, const uint8_t *data, size_t data_size)
{
  // Prefer magic-byte detection over extension to handle mislabelled
  // resources inside EPUB containers.
  bool looks_jpeg = (data_size > 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF);
  bool looks_png = (data_size > 4 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G');

  if (looks_jpeg)
  {
    if (!jpeg_helper)
    {
      jpeg_helper = new JPEGHelper();
    }
    return jpeg_helper;
  }
  if (looks_png)
  {
    if (!png_helper)
    {
      png_helper = new PNGHelper();
    }
    return png_helper;
  }

  // Fallback to file extension if magic bytes are inconclusive. The
  // comparison is done case-insensitively so that resources such as
  // sleep images from "/fs/Pics" still decode correctly even when
  // their extensions are upper-case or mixed-case.
  std::string lower = filename;
  for (char &c : lower)
  {
    if (c >= 'A' && c <= 'Z')
    {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }

  if (lower.find(".jpg") != std::string::npos ||
      lower.find(".jpeg") != std::string::npos)
  {
    if (!jpeg_helper)
    {
      jpeg_helper = new JPEGHelper();
    }
    return jpeg_helper;
  }
  if (lower.find(".png") != std::string::npos)
  {
    if (!png_helper)
    {
      png_helper = new PNGHelper();
    }
    return png_helper;
  }
  return nullptr;
}

void Renderer::draw_image(const std::string &filename, const uint8_t *data, size_t data_size, int x, int y, int width, int height)
{
  ImageHelper *helper = get_image_helper(filename, data, data_size);
  if (!helper ||
      !helper->render(data, data_size, this, x, y, width, height))
  {
    // If an image cannot be decoded or has an unknown type, do not draw
    // any generic cover-style placeholder. Callers that need a fallback
    // (such as the library views) are responsible for drawing their own
    // title cards or other UI elements in the target region.
    (void)image_placeholder_enabled;
    return;
  }
}

bool Renderer::get_image_size(const std::string &filename, const uint8_t *data, size_t data_size, int *width, int *height)
{
  ImageHelper *helper = get_image_helper(filename, data, data_size);
  if (helper && helper->get_size(data, data_size, width, height))
  {
    return true;
  }
  // just provide a dummy height and width so we can do a placeholder
  // for this unknown image typew
  *width = std::min(get_page_width(), get_page_height());
  *height = *width;
  return false;
}

void Renderer::draw_text_box(const std::string &text, int x, int y, int width, int height, bool bold, bool italic)
{
  int length = text.length();
  // fit the text into the box
  int start = 0;
  int end = 1;
  int ypos = 0;
  while (start < length && ypos + get_line_height() < height)
  {
    while (end < length && get_text_width(text.substr(start, end - start).c_str(), bold, italic) < width)
    {
      end++;
    }
    if (get_text_width(text.substr(start, end - start).c_str(), bold, italic) > width)
    {
      end--;
    }
    draw_text(x, y + ypos, text.substr(start, end - start).c_str(), bold, italic);
    ypos += get_line_height();
    start = end;
    end = start + 1;
  }
}
