#include "PaperS3TouchControls.h"

#include "Renderer/Renderer.h"
#include "EpubList/State.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <driver/i2c.h>

// Paper S3 GT911 touch is configured as a 540x960 coordinate space (x,y). We
// always interpret touches in this space and map them onto the 540x960 logical
// page used by the rotated 960x540 e-paper panel.

static const char *TAG = "PaperS3Touch";

// Mirror the UIState enum defined in main.cpp so we can adapt
// touch behaviour based on the current screen.
typedef enum
{
  SELECTING_EPUB,
  SELECTING_TABLE_CONTENTS,
  READING_EPUB,
  READING_MENU
} UIState;

extern UIState ui_state;
extern EpubListState epub_list_state;
extern EpubTocState epub_index_state;
extern int reader_menu_selected;
extern bool invert_tap_zones;
extern bool reader_menu_advanced;

// PaperS3 GT911 wiring (from M5GFX / M5PaperS3 config)
static const gpio_num_t PAPERS3_GT911_SDA_GPIO = GPIO_NUM_41;
static const gpio_num_t PAPERS3_GT911_SCL_GPIO = GPIO_NUM_42;
static const gpio_num_t PAPERS3_GT911_INT_GPIO = GPIO_NUM_48;
static const i2c_port_t PAPERS3_GT911_I2C_PORT = I2C_NUM_0;

// Gesture sensitivity profile (updated via set_gesture_profile).
static uint16_t s_swipe_threshold = 100;
static uint16_t s_longpress_move_threshold = 30;
static uint32_t s_longpress_ms = 600;

static esp_err_t gt911_write_reg(uint8_t addr, uint16_t reg, const uint8_t *data, size_t len)
{
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  if (!cmd)
  {
    return ESP_FAIL;
  }

  uint8_t reg_hi = reg >> 8;
  uint8_t reg_lo = reg & 0xFF;

  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write_byte(cmd, reg_hi, true);
  i2c_master_write_byte(cmd, reg_lo, true);
  if (data && len)
  {
    i2c_master_write(cmd, (uint8_t *)data, len, true);
  }
  i2c_master_stop(cmd);

  esp_err_t ret = i2c_master_cmd_begin(PAPERS3_GT911_I2C_PORT, cmd, pdMS_TO_TICKS(100));
  i2c_cmd_link_delete(cmd);
  return ret;
}

static esp_err_t gt911_read_reg(uint8_t addr, uint16_t reg, uint8_t *data, size_t len)
{
  if (!data || !len)
  {
    return ESP_ERR_INVALID_ARG;
  }

  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  if (!cmd)
  {
    return ESP_FAIL;
  }

  uint8_t reg_hi = reg >> 8;
  uint8_t reg_lo = reg & 0xFF;

  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write_byte(cmd, reg_hi, true);
  i2c_master_write_byte(cmd, reg_lo, true);

  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);

  if (len > 1)
  {
    i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
  }
  i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
  i2c_master_stop(cmd);

  esp_err_t ret = i2c_master_cmd_begin(PAPERS3_GT911_I2C_PORT, cmd, pdMS_TO_TICKS(100));
  i2c_cmd_link_delete(cmd);
  return ret;
}

PaperS3TouchControls::PaperS3TouchControls(Renderer *renderer, ActionCallback_t on_action)
    : on_action(on_action), renderer(renderer)
{
  // Configure I2C using the legacy API to avoid conflicts with driver_ng.
  i2c_config_t conf = {};
  conf.mode = I2C_MODE_MASTER;
  conf.sda_io_num = PAPERS3_GT911_SDA_GPIO;
  conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
  conf.scl_io_num = PAPERS3_GT911_SCL_GPIO;
  conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
  conf.master.clk_speed = 400000;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 3, 0)
  conf.clk_flags = 0;
#endif

  esp_err_t err = i2c_param_config(PAPERS3_GT911_I2C_PORT, &conf);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "i2c_param_config failed: %d", err);
    return;
  }

  err = i2c_driver_install(PAPERS3_GT911_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
  {
    ESP_LOGE(TAG, "i2c_driver_install failed: %d", err);
    return;
  }

  // Try to detect GT911 at 0x14 then 0x5D by reading a config byte.
  uint8_t buf = 0;
  if (gt911_read_reg(0x14, 0x8140, &buf, 1) == ESP_OK)
  {
    i2c_addr = 0x14;
    driver_ok = true;
  }
  else if (gt911_read_reg(0x5D, 0x8140, &buf, 1) == ESP_OK)
  {
    i2c_addr = 0x5D;
    driver_ok = true;
  }
  else
  {
    ESP_LOGE(TAG, "GT911 not found on I2C bus");
    driver_ok = false;
    return;
  }

  ESP_LOGI(TAG, "GT911 detected at 0x%02X", i2c_addr);

  // Create a polling task to read touch events.
  if (xTaskCreatePinnedToCore(&PaperS3TouchControls::touchTask, "papers3_touch", 4096, this, 1, nullptr, 1) != pdPASS)
  {
    ESP_LOGE(TAG, "Failed to create touch task");
    driver_ok = false;
  }
}

void PaperS3TouchControls::set_gesture_profile(int profile_index)
{
  switch (profile_index)
  {
  case 0: // low sensitivity: require larger movement/press
    s_swipe_threshold = 120;
    s_longpress_move_threshold = 40;
    s_longpress_ms = 800;
    break;
  case 2: // high sensitivity
    s_swipe_threshold = 70;
    s_longpress_move_threshold = 20;
    s_longpress_ms = 500;
    break;
  case 1:
  default: // medium (default)
    s_swipe_threshold = 100;
    s_longpress_move_threshold = 30;
    s_longpress_ms = 600;
    break;
  }
}

void PaperS3TouchControls::render(Renderer *renderer)
{
  (void)renderer;
}

void PaperS3TouchControls::renderPressedState(Renderer *renderer, UIAction action, bool state)
{
  (void)renderer;
  (void)action;
  (void)state;
}

void PaperS3TouchControls::touchTask(void *param)
{
  auto *self = static_cast<PaperS3TouchControls *>(param);
  self->loop();
}

bool PaperS3TouchControls::readTouchPoint(uint16_t *x, uint16_t *y, uint8_t *points)
{
  if (!driver_ok || !x || !y)
  {
    return false;
  }

  // GT911 status register at 0x814E
  uint8_t status = 0;
  if (gt911_read_reg(i2c_addr, 0x814E, &status, 1) != ESP_OK)
  {
    return false;
  }

  if (!(status & 0x80))
  {
    // No new data
    return false;
  }

  uint8_t count = status & 0x0F;
  if (count == 0)
  {
    // Clear the status flag
    uint8_t zero = 0;
    gt911_write_reg(i2c_addr, 0x814E, &zero, 1);
    return false;
  }

  if (points)
  {
    *points = count;
  }

  // Read the first touch point from 0x8150 (X, Y, etc.).
  uint8_t data[4] = {0};
  if (gt911_read_reg(i2c_addr, 0x8150, data, sizeof(data)) != ESP_OK)
  {
    return false;
  }

  *x = (uint16_t)(data[1] << 8 | data[0]);
  *y = (uint16_t)(data[3] << 8 | data[2]);

  // Clear the status flag so the controller can update.
  uint8_t zero = 0;
  gt911_write_reg(i2c_addr, 0x814E, &zero, 1);

  return true;
}

UIAction PaperS3TouchControls::mapTapToAction(uint16_t x, uint16_t y)
{
  // The GT911 on PaperS3 is configured by M5GFX as 540x960.
  const uint16_t touch_width = 540;
  const uint16_t touch_height = 960;

  if (x >= touch_width || y >= touch_height)
  {
    return NONE;
  }

  int epd_width = renderer->get_page_width();
  int epd_height = renderer->get_page_height();
  if (epd_width <= 0 || epd_height <= 0)
  {
    return NONE;
  }

  int logical_x = static_cast<int>(x) * epd_width / touch_width;
  int logical_y = static_cast<int>(y) * epd_height / touch_height;

  int bottom_bar_height = EPUB_LIST_BOTTOM_BAR_HEIGHT;
  int bar_top = epd_height - bottom_bar_height;
  if (bar_top < 0)
  {
    bar_top = epd_height;
  }

  // On the EPUB list screen, interpret taps on the content area as
  // selecting a book (grid or list) and taps on the bottom bar as
  // page navigation or view toggle.
  if (ui_state == SELECTING_EPUB && epub_list_state.is_loaded && epub_list_state.num_epubs > 0)
  {
    int content_height = epd_height;
    if (bottom_bar_height > 0 && bottom_bar_height < epd_height)
    {
      content_height = epd_height - bottom_bar_height;
    }

    // Bottom bar: "<<      <      Page X of Y      >      >>" textual
    // navigation. Use the same geometry as the renderer so the touch
    // hitboxes track the visual arrow positions. Double arrows (outer
    // zones) move by a page; single arrows (inner zones) move selection
    // by one item.
    if (bottom_bar_height > 0 && logical_y >= bar_top)
    {
      int items_per_page = epub_list_state.use_grid_view ? (EPUB_GRID_ROWS * EPUB_GRID_COLUMNS) : EPUB_LIST_ITEMS_PER_PAGE;
      if (items_per_page <= 0)
      {
        items_per_page = 1;
      }

      // Match the five equal-width navigation regions rendered in
      // EpubList::render(): [<<] [<] [Page X of Y] [>] [>>].
      int columns = 5;
      int col_width = epd_width / columns;
      if (col_width <= 0)
      {
        col_width = 1;
      }

      int ld_zone_start = 0;
      int ld_zone_end = ld_zone_start + col_width;

      int ls_zone_start = ld_zone_end;
      int ls_zone_end = ls_zone_start + col_width;

      int center_zone_start = ls_zone_end;
      int center_zone_end = center_zone_start + col_width;

      int rs_zone_start = center_zone_end;
      int rs_zone_end = rs_zone_start + col_width;

      int rd_zone_start = rs_zone_end;
      int rd_zone_end = epd_width; // consume any remainder to reach the edge

      if (logical_x < ld_zone_end)
      {
        // "<<" – jump back by one page.
        int new_index = epub_list_state.selected_item - items_per_page;
        if (new_index < 0)
        {
          new_index = 0;
        }
        epub_list_state.selected_item = new_index;
        return LAST_INTERACTION;
      }
      if (logical_x < ls_zone_end)
      {
        // "<" – move selection to previous item.
        int new_index = epub_list_state.selected_item - 1;
        if (new_index < 0)
        {
          new_index = 0;
        }
        epub_list_state.selected_item = new_index;
        return LAST_INTERACTION;
      }
      if (logical_x < center_zone_end)
      {
        // Taps on the "Page X of Y" text do not change selection.
        return NONE;
      }
      if (logical_x < rs_zone_end)
      {
        // ">" – move selection to next item.
        int new_index = epub_list_state.selected_item + 1;
        if (new_index >= epub_list_state.num_epubs)
        {
          new_index = epub_list_state.num_epubs - 1;
        }
        if (new_index < 0)
        {
          new_index = 0;
        }
        epub_list_state.selected_item = new_index;
        return LAST_INTERACTION;
      }
      if (logical_x < rd_zone_end)
      {
        // ">>" – jump forward by one page.
        int new_index = epub_list_state.selected_item + items_per_page;
        if (new_index >= epub_list_state.num_epubs)
        {
          new_index = epub_list_state.num_epubs - 1;
        }
        if (new_index < 0)
        {
          new_index = 0;
        }
        epub_list_state.selected_item = new_index;
        return LAST_INTERACTION;
      }
      return NONE;
    }

    // Content area: tap a book cover or row to open.
    if (logical_y >= content_height)
    {
      return NONE;
    }

    int items_per_page = epub_list_state.use_grid_view ? (EPUB_GRID_ROWS * EPUB_GRID_COLUMNS) : EPUB_LIST_ITEMS_PER_PAGE;
    if (items_per_page <= 0)
    {
      items_per_page = 1;
    }
    int current_page = 0;
    if (epub_list_state.num_epubs > 0)
    {
      current_page = epub_list_state.selected_item / items_per_page;
    }

    if (epub_list_state.use_grid_view)
    {
      int cell_width = epd_width / EPUB_GRID_COLUMNS;
      int cell_height = content_height / EPUB_GRID_ROWS;
      if (cell_width <= 0 || cell_height <= 0)
      {
        return NONE;
      }
      int row = logical_y / cell_height;
      int col = logical_x / cell_width;
      if (row < 0)
      {
        row = 0;
      }
      if (row >= EPUB_GRID_ROWS)
      {
        row = EPUB_GRID_ROWS - 1;
      }
      if (col < 0)
      {
        col = 0;
      }
      if (col >= EPUB_GRID_COLUMNS)
      {
        col = EPUB_GRID_COLUMNS - 1;
      }
      int index_in_page = row * EPUB_GRID_COLUMNS + col;
      if (index_in_page >= items_per_page)
      {
        return NONE;
      }
      int index = current_page * items_per_page + index_in_page;
      if (index >= epub_list_state.num_epubs)
      {
        return NONE;
      }
      epub_list_state.selected_item = index;
      return SELECT;
    }
    else
    {
      if (content_height <= 0)
      {
        return NONE;
      }
      int cell_height = content_height / EPUB_LIST_ITEMS_PER_PAGE;
      if (cell_height <= 0)
      {
        cell_height = 1;
      }
      int row_in_page = logical_y / cell_height;
      if (row_in_page < 0)
      {
        row_in_page = 0;
      }
      if (row_in_page >= EPUB_LIST_ITEMS_PER_PAGE)
      {
        row_in_page = EPUB_LIST_ITEMS_PER_PAGE - 1;
      }

      int index = current_page * EPUB_LIST_ITEMS_PER_PAGE + row_in_page;
      if (index >= epub_list_state.num_epubs)
      {
        index = epub_list_state.num_epubs - 1;
      }
      if (index < 0)
      {
        index = 0;
      }
      epub_list_state.selected_item = index;
      return SELECT;
    }
  }

  // In the table-of-contents view, interpret taps on the content
  // area as selecting the tapped TOC entry (row) and opening that
  // chapter, and taps on the bottom bar as paging the TOC.
  if (ui_state == SELECTING_TABLE_CONTENTS)
  {
    int content_height = epd_height;
    if (bottom_bar_height > 0 && bottom_bar_height < epd_height)
    {
      content_height = epd_height - bottom_bar_height;
    }

    if (bottom_bar_height > 0 && logical_y >= bar_top)
    {
      const int items_per_page = EPUB_TOC_ITEMS_PER_PAGE;
      if (epub_index_state.num_items <= 0)
      {
        return NONE;
      }

      // Match the five equal-width navigation regions rendered in
      // EpubToc::render(): [<<] [<] [Page X of Y] [>] [>>].
      int columns = 5;
      int col_width = epd_width / columns;
      if (col_width <= 0)
      {
        col_width = 1;
      }

      int ld_zone_start = 0;
      int ld_zone_end = ld_zone_start + col_width;

      int ls_zone_start = ld_zone_end;
      int ls_zone_end = ls_zone_start + col_width;

      int center_zone_start = ls_zone_end;
      int center_zone_end = center_zone_start + col_width;

      int rs_zone_start = center_zone_end;
      int rs_zone_end = rs_zone_start + col_width;

      int rd_zone_start = rs_zone_end;
      int rd_zone_end = epd_width; // consume any remainder to reach the edge

      if (logical_x < ld_zone_end)
      {
        // "<<" – jump back by one page.
        int new_index = epub_index_state.selected_item - items_per_page;
        if (new_index < 0)
        {
          new_index = 0;
        }
        epub_index_state.selected_item = new_index;
        return LAST_INTERACTION;
      }
      if (logical_x < ls_zone_end)
      {
        // "<" – move selection to previous TOC item.
        int new_index = epub_index_state.selected_item - 1;
        if (new_index < 0)
        {
          new_index = 0;
        }
        epub_index_state.selected_item = new_index;
        return LAST_INTERACTION;
      }
      if (logical_x < center_zone_end)
      {
        // Taps on the "Page X of Y" text do not change selection.
        return NONE;
      }
      if (logical_x < rs_zone_end)
      {
        // ">" – move selection to next TOC item.
        int new_index = epub_index_state.selected_item + 1;
        if (new_index >= epub_index_state.num_items)
        {
          new_index = epub_index_state.num_items - 1;
        }
        if (new_index < 0)
        {
          new_index = 0;
        }
        epub_index_state.selected_item = new_index;
        return LAST_INTERACTION;
      }
      if (logical_x < rd_zone_end)
      {
        // ">>" – jump forward by one page.
        int new_index = epub_index_state.selected_item + items_per_page;
        if (new_index >= epub_index_state.num_items)
        {
          new_index = epub_index_state.num_items - 1;
        }
        if (new_index < 0)
        {
          new_index = 0;
        }
        epub_index_state.selected_item = new_index;
        return LAST_INTERACTION;
      }
      return NONE;
    }

    if (epub_index_state.num_items <= 0)
    {
      return NONE;
    }

    if (logical_y >= content_height)
    {
      return NONE;
    }

    int cell_height = content_height / EPUB_TOC_ITEMS_PER_PAGE;
    if (cell_height <= 0)
    {
      cell_height = 1;
    }
    int row_in_page = logical_y / cell_height;
    if (row_in_page < 0)
    {
      row_in_page = 0;
    }
    if (row_in_page >= EPUB_TOC_ITEMS_PER_PAGE)
    {
      row_in_page = EPUB_TOC_ITEMS_PER_PAGE - 1;
    }
    int current_page = epub_index_state.selected_item / EPUB_TOC_ITEMS_PER_PAGE;
    int index = current_page * EPUB_TOC_ITEMS_PER_PAGE + row_in_page;
    if (index >= epub_index_state.num_items)
    {
      index = epub_index_state.num_items - 1;
    }
    if (index < 0)
    {
      index = 0;
    }
    epub_index_state.selected_item = index;
    return SELECT;
  }

  // While reading, keep simple left/right tap navigation for
  // page turns, and add a top tap region that opens the in-book
  // menu.
  if (ui_state == READING_EPUB)
  {
    int top_zone = epd_height / 6;
    if (top_zone <= 0)
    {
      top_zone = epd_height / 4;
    }
    if (logical_y < top_zone)
    {
      // A tap at the top of the screen opens the in-book menu.
      return SELECT;
    }

    uint16_t left_zone = touch_width / 3;
    uint16_t right_zone = (touch_width * 2) / 3;

    if (x < left_zone)
    {
      return invert_tap_zones ? DOWN : UP;
    }
    if (x >= right_zone)
    {
      return invert_tap_zones ? UP : DOWN;
    }
    return SELECT;
  }

  if (ui_state == READING_MENU)
  {
    int page_width = renderer->get_page_width();
    int page_height = renderer->get_page_height();
    int line_height = renderer->get_line_height();
    if (line_height <= 0)
    {
      line_height = 20;
    }
    if (page_height <= 0)
    {
      page_height = line_height * 8;
    }
    if (page_width <= 0)
    {
      page_width = 400;
    }

    int bottom_bar_height = EPUB_LIST_BOTTOM_BAR_HEIGHT;
    int bar_top = page_height - bottom_bar_height;
    if (bar_top < 0)
    {
      bar_top = page_height;
    }
    int content_height = page_height;
    if (bottom_bar_height > 0 && bottom_bar_height < page_height)
    {
      content_height = page_height - bottom_bar_height;
    }

    // Mirror the reader menu item counts and paging used in
    // renderReaderMenu(): 6 basic items, 10 advanced items, with a
    // maximum of 6 visible per page (EPUB_TOC_ITEMS_PER_PAGE).
    int items_total = reader_menu_advanced ? 10 : 6;
    if (items_total <= 0)
    {
      return NONE;
    }
    int items_per_page = EPUB_TOC_ITEMS_PER_PAGE;
    if (items_per_page <= 0 || items_per_page > items_total)
    {
      items_per_page = items_total;
    }

    if (reader_menu_selected < 0)
    {
      reader_menu_selected = 0;
    }
    if (reader_menu_selected >= items_total)
    {
      reader_menu_selected = items_total - 1;
    }

    int total_pages = (items_total + items_per_page - 1) / items_per_page;
    if (total_pages < 1)
    {
      total_pages = 1;
    }
    int current_page = reader_menu_selected / items_per_page;
    if (current_page < 0)
    {
      current_page = 0;
    }
    if (current_page >= total_pages)
    {
      current_page = total_pages - 1;
    }

    int start_index = current_page * items_per_page;
    int end_index = start_index + items_per_page;
    if (end_index > items_total)
    {
      end_index = items_total;
    }
    int visible_count = end_index - start_index;
    if (visible_count <= 0)
    {
      return NONE;
    }

    int button_vertical_padding = line_height / 4;
    if (button_vertical_padding < 2)
    {
      button_vertical_padding = 2;
    }
    int button_height = line_height + button_vertical_padding * 2;
    int button_spacing = line_height / 4;
    if (button_spacing < 2)
    {
      button_spacing = 2;
    }

    // Bottom bar navigation: "<<  <  X / Y  >  >>" using the same
    // five equal-width regions as EpubList/EpubToc. Double arrows
    // jump by a page; single arrows move selection by one item.
    if (bottom_bar_height > 0 && logical_y >= bar_top)
    {
      int columns = 5;
      int col_width = page_width / columns;
      if (col_width <= 0)
      {
        col_width = 1;
      }

      int ld_zone_start = 0;
      int ld_zone_end = ld_zone_start + col_width;

      int ls_zone_start = ld_zone_end;
      int ls_zone_end = ls_zone_start + col_width;

      int center_zone_start = ls_zone_end;
      int center_zone_end = center_zone_start + col_width;

      int rs_zone_start = center_zone_end;
      int rs_zone_end = rs_zone_start + col_width;

      int rd_zone_start = rs_zone_end;
      int rd_zone_end = page_width;

      if (logical_x < ld_zone_end)
      {
        // "<<" – in the advanced reader menu, act as a Back
        // control: return to the basic menu. In the basic menu,
        // retain the page-jump semantics even though everything fits
        // on a single page.
        if (reader_menu_advanced)
        {
          reader_menu_advanced = false;
          reader_menu_selected = 0;
          return LAST_INTERACTION;
        }

        int new_index = reader_menu_selected - items_per_page;
        if (new_index < 0)
        {
          new_index = 0;
        }
        reader_menu_selected = new_index;
        return LAST_INTERACTION;
      }
      if (logical_x < ls_zone_end)
      {
        // "<" – move selection to previous item.
        int new_index = reader_menu_selected - 1;
        if (new_index < 0)
        {
          new_index = 0;
        }
        reader_menu_selected = new_index;
        return LAST_INTERACTION;
      }
      if (logical_x < center_zone_end)
      {
        // Taps on the page indicator do not change selection.
        return NONE;
      }
      if (logical_x < rs_zone_end)
      {
        // ">" – move selection to next item.
        int new_index = reader_menu_selected + 1;
        if (new_index >= items_total)
        {
          new_index = items_total - 1;
        }
        if (new_index < 0)
        {
          new_index = 0;
        }
        reader_menu_selected = new_index;
        return LAST_INTERACTION;
      }
      if (logical_x < rd_zone_end)
      {
        // ">>" – jump forward by one page.
        int new_index = reader_menu_selected + items_per_page;
        if (new_index >= items_total)
        {
          new_index = items_total - 1;
        }
        if (new_index < 0)
        {
          new_index = 0;
        }
        reader_menu_selected = new_index;
        return LAST_INTERACTION;
      }
      return NONE;
    }

    // Content area: tap a visible menu button to select it.
    if (logical_y >= content_height)
    {
      return NONE;
    }

    int container_height = visible_count * button_height + (visible_count - 1) * button_spacing;
    int container_y = (content_height - container_height) / 2;
    if (container_y < 0)
    {
      container_y = 0;
    }

    if (logical_y < container_y || logical_y >= container_y + container_height)
    {
      return NONE;
    }

    int offset_y = logical_y - container_y;
    for (int i = 0; i < visible_count; i++)
    {
      int button_top = i * (button_height + button_spacing);
      int button_bottom = button_top + button_height;
      if (offset_y >= button_top && offset_y < button_bottom)
      {
        int item_index = start_index + i;
        if (item_index >= items_total)
        {
          item_index = items_total - 1;
        }
        if (item_index < 0)
        {
          item_index = 0;
        }
        reader_menu_selected = item_index;
        return SELECT;
      }
    }
    return NONE;
  }

  return NONE;
}

UIAction PaperS3TouchControls::mapSwipeUpToAction(uint16_t start_x, uint16_t start_y, uint16_t end_x, uint16_t end_y, uint8_t max_points)
{
  (void)end_x;
  (void)end_y;

  // The GT911 on PaperS3 is configured by M5GFX as 540x960.
  const uint16_t touch_width = 540;
  const uint16_t touch_height = 960;

  if (start_x >= touch_width || start_y >= touch_height)
  {
    return NONE;
  }

  // From the library view (SELECTING_EPUB), treat any two-finger swipe
  // up anywhere on the screen as a request to open the reader menu
  // (advanced settings) without first opening a book.
  if (ui_state == SELECTING_EPUB && max_points >= 2)
  {
    return OPEN_READER_MENU;
  }

  // When reading, interpret a swipe up as a request to open
  // navigation: map it to SELECT, which handleEpub() already
  // interprets as "open the reader menu".
  if (ui_state == READING_EPUB)
  {
    return SELECT;
  }

  return NONE;
}

UIAction PaperS3TouchControls::mapSwipeDownToAction(uint16_t start_x, uint16_t start_y, uint16_t end_x, uint16_t end_y, uint8_t max_points)
{
  (void)end_x;
  (void)end_y;

  // The GT911 on PaperS3 is configured by M5GFX as 540x960.
  const uint16_t touch_width = 540;
  const uint16_t touch_height = 960;

  if (start_x >= touch_width || start_y >= touch_height)
  {
    return NONE;
  }

  // While reading, interpret any two-finger swipe down anywhere on the
  // screen as a request to force a full-page refresh. Single-finger
  // vertical swipes remain unused so they do not interfere with
  // existing page-turn gestures.
  if (ui_state == READING_EPUB && max_points >= 2)
  {
    return REFRESH_PAGE;
  }

  return NONE;
}

void PaperS3TouchControls::loop()
{
  // Threshold in touch-coordinate pixels to distinguish a swipe
  // from a tap. The GT911 reports 540x960.
  const uint16_t swipe_threshold = s_swipe_threshold;
  const uint16_t longpress_move_threshold = s_longpress_move_threshold;
  const uint32_t longpress_ms = s_longpress_ms;

  uint16_t start_x = 0;
  uint16_t start_y = 0;
  uint16_t current_x = 0;
  uint16_t current_y = 0;
  // Track the maximum number of simultaneous touch points seen
  // during the current gesture so we can distinguish two-finger
  // swipes from single-finger swipes.
  uint8_t max_points = 0;

  while (true)
  {
    uint16_t x = 0;
    uint16_t y = 0;
    uint8_t points = 0;

    if (readTouchPoint(&x, &y, &points))
    {
      if (!touch_active)
      {
        // First contact
        touch_active = true;
        touch_start_tick = xTaskGetTickCount();
        start_x = current_x = x;
        start_y = current_y = y;
        max_points = points;
      }
      else
      {
        // Update current position while the finger moves.
        current_x = x;
        current_y = y;
        if (points > max_points)
        {
          max_points = points;
        }
      }
    }
    else
    {
      if (touch_active)
      {
        // Touch has just ended – decide between tap and swipe.
        int dx = static_cast<int>(current_x) - static_cast<int>(start_x);
        int dy = static_cast<int>(start_y) - static_cast<int>(current_y); // positive when moving up
        int abs_dx = dx >= 0 ? dx : -dx;
        int abs_dy = dy >= 0 ? dy : -dy;

        TickType_t end_tick = xTaskGetTickCount();
        uint32_t dt_ms = (end_tick - touch_start_tick) * portTICK_PERIOD_MS;

        UIAction action = NONE;

        // Long-press: minimal movement and held for longpress_ms.
        if (dt_ms >= longpress_ms && abs_dx <= static_cast<int>(longpress_move_threshold) &&
            abs_dy <= static_cast<int>(longpress_move_threshold))
        {
          action = mapLongPressToAction(start_x, start_y);
        }
        // Horizontal swipe: chapter-level navigation while reading.
        else if (abs_dx > abs_dy && abs_dx > static_cast<int>(swipe_threshold))
        {
          if (ui_state == READING_EPUB)
          {
            action = (dx > 0) ? NEXT_SECTION : PREV_SECTION;
          }
        }
        // Vertical swipe up/down.
        else if (dy > static_cast<int>(swipe_threshold))
        {
          action = mapSwipeUpToAction(start_x, start_y, current_x, current_y, max_points);
        }
        else if (dy < -static_cast<int>(swipe_threshold))
        {
          action = mapSwipeDownToAction(start_x, start_y, current_x, current_y, max_points);
        }
        else
        {
          action = mapTapToAction(start_x, start_y);
        }

        touch_active = false;
        max_points = 0;
        last_action = action;
        ESP_LOGD(TAG, "Touch at %u,%u (end %u,%u) -> dy=%d, action %d", start_x, start_y, current_x, current_y, dy, (int)action);
        if (action != NONE && on_action)
        {
          on_action(action);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

UIAction PaperS3TouchControls::mapLongPressToAction(uint16_t x, uint16_t y)
{
  // The GT911 on PaperS3 is configured by M5GFX as 540x960.
  const uint16_t touch_width = 540;
  const uint16_t touch_height = 960;

  if (x >= touch_width || y >= touch_height)
  {
    return NONE;
  }

  // While reading, long-press bottom-left/right for previous/next
  // section (chapter), KOReader-style.
  if (ui_state == READING_EPUB)
  {
    uint16_t top_zone = touch_height / 3;
    uint16_t bottom_zone = (touch_height * 2) / 3;
    uint16_t left_center = touch_width / 3;
    uint16_t right_center = (touch_width * 2) / 3;

    // Long-press top-center toggles the status bar visibility.
    if (y < top_zone && x >= left_center && x < right_center)
    {
      return TOGGLE_STATUS_BAR;
    }

    if (y >= bottom_zone)
    {
      uint16_t mid_x = touch_width / 2;
      if (x < mid_x)
      {
        return PREV_SECTION;
      }
      else
      {
        return NEXT_SECTION;
      }
    }
    return NONE;
  }

  // In the EPUB list, long-pressing a row behaves like tapping it:
  // select that row and open it.
  if (ui_state == SELECTING_EPUB && epub_list_state.is_loaded && epub_list_state.num_epubs > 0)
  {
    int epd_height = renderer->get_page_height();
    if (epd_height <= 0)
    {
      return NONE;
    }
    int logical_y = static_cast<int>(y) * epd_height / touch_height;
    const int items_per_page = 5;
    int cell_height = epd_height / items_per_page;
    if (cell_height <= 0)
    {
      cell_height = 1;
    }
    int row_in_page = logical_y / cell_height;
    if (row_in_page < 0)
    {
      row_in_page = 0;
    }
    if (row_in_page >= items_per_page)
    {
      row_in_page = items_per_page - 1;
    }

    int current_page = epub_list_state.selected_item / items_per_page;
    int index = current_page * items_per_page + row_in_page;
    if (index >= epub_list_state.num_epubs)
    {
      index = epub_list_state.num_epubs - 1;
    }
    if (index < 0)
    {
      index = 0;
    }
    epub_list_state.selected_item = index;
    return SELECT;
  }

  // In the TOC, long-press a row to open that chapter directly.
  if (ui_state == SELECTING_TABLE_CONTENTS && epub_index_state.num_items > 0)
  {
    int epd_height = renderer->get_page_height();
    if (epd_height <= 0)
    {
      return NONE;
    }
    int logical_y = static_cast<int>(y) * epd_height / touch_height;
    const int items_per_page = 6;
    int cell_height = epd_height / items_per_page;
    if (cell_height <= 0)
    {
      cell_height = 1;
    }
    int row_in_page = logical_y / cell_height;
    if (row_in_page < 0)
    {
      row_in_page = 0;
    }
    if (row_in_page >= items_per_page)
    {
      row_in_page = items_per_page - 1;
    }
    int current_page = epub_index_state.selected_item / items_per_page;
    int index = current_page * items_per_page + row_in_page;
    if (index >= epub_index_state.num_items)
    {
      index = epub_index_state.num_items - 1;
    }
    if (index < 0)
    {
      index = 0;
    }
    epub_index_state.selected_item = index;
    return SELECT;
  }

  return NONE;
}
