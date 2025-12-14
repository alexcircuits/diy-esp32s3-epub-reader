#pragma once
#include <esp_log.h>

#if defined(BOARD_TYPE_PAPER_S3)
#include <epdiy.h>
#include <epd_highlevel.h>
// Custom epdiy board definition for M5Stack Paper S3
extern const EpdBoardDefinition paper_s3_board;
#else
#include <epd_driver.h>
#include <epd_highlevel.h>
#endif

#include <math.h>
#include "EpdiyFrameBufferRenderer.h"
#include "miniz.h"

class EpdiyRenderer : public EpdiyFrameBufferRenderer
{
private:
  EpdiyHighlevelState m_hl;

public:
  EpdiyRenderer(
      const EpdFont *regular_font,
      const EpdFont *bold_font,
      const EpdFont *italic_font,
      const EpdFont *bold_italic_font,
      const uint8_t *busy_icon,
      int busy_icon_width,
      int busy_icon_height)
      : EpdiyFrameBufferRenderer(regular_font, bold_font, italic_font, bold_italic_font, busy_icon, busy_icon_width, busy_icon_height)
  {
    // start up the EPD
#if defined(BOARD_TYPE_PAPER_S3)
    // For Paper S3 we use the new epdiy API with a custom board definition
    epd_set_board(&paper_s3_board);
    epd_init(epd_current_board(), &ED047TC2, EPD_OPTIONS_DEFAULT);
    // The C fallback for the ESP32-S3 LUT path is slower than the original
    // vector assembly, so a 20 MHz pixel clock can cause line buffer underruns
    // (EPD_DRAW_EMPTY_LINE_QUEUE). Run the LCD at 5 MHz instead for stability
    // on lower CPU clock configurations.
    epd_set_lcd_pixel_clock_MHz(5);
#else
    // Legacy epdiy API used by ESP32-based boards
    epd_init(EPD_OPTIONS_DEFAULT);
#endif

    m_hl = epd_hl_init(EPD_BUILTIN_WAVEFORM);
    // first set full screen to white
    epd_hl_set_all_white(&m_hl);
    m_frame_buffer = epd_hl_get_framebuffer(&m_hl);

#if !defined(CONFIG_EPD_BOARD_REVISION_LILYGO_T5_47) || defined(BOARD_TYPE_PAPER_S3)
    epd_poweron();
#endif
  }
  ~EpdiyRenderer()
  {
    epd_deinit();
  }
  void flush_display()
  {
    epd_hl_update_screen(&m_hl, needs_gray_flush ? MODE_GC16 : MODE_DU, temperature);
    needs_gray_flush = false;
  }
  void flush_area(int x, int y, int width, int height)
  {
    epd_hl_update_area(&m_hl, MODE_DU, temperature, {.x = x, .y = y, .width = width, .height = height});
  }
  virtual void reset()
  {
    ESP_LOGI("EPD", "Full clear");
    epd_fullclear(&m_hl, temperature);
  };
  // deep sleep helper - retrieve any state from disk after wake
  virtual bool hydrate()
  {
    ESP_LOGI("EPD", "Hydrating EPD");
    if (EpdiyFrameBufferRenderer::hydrate())
    {
      // just memcopy the front buffer to the back buffer - they should be exactly the same
      memcpy(m_hl.back_fb, m_frame_buffer, EPD_WIDTH * EPD_HEIGHT / 2);
      ESP_LOGI("EPD", "Hydrated EPD");
      return true;
    }
    else
    {
      ESP_LOGI("EPD", "Hydrate EPD failed");
      reset();
      return false;
    }
  };
};