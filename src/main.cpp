#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <ctype.h>
#include <esp_random.h>
#include <memory>
#include <span>
#include <ranges>
#include <vector>
#include "config.h"
#include "EpubList/Epub.h"
#include "EpubList/EpubList.h"
#include "EpubList/EpubReader.h"
#include "EpubList/EpubToc.h"
#include <RubbishHtmlParser/RubbishHtmlParser.h>
#include "boards/Board.h"
#include "boards/controls/PaperS3TouchControls.h"

#ifdef USE_FREETYPE
#include "Renderer/EpdiyFrameBufferRenderer.h"
#include "Renderer/FreeTypeFont.h"

#if defined(BOARD_TYPE_PAPER_S3)
// Global FreeType font instance used by the Paper S3 renderer.
static std::unique_ptr<FreeTypeFont> g_paper_s3_ft_font;

static void init_freetype_for_paper_s3(Renderer *renderer)
{
  if (g_paper_s3_ft_font)
  {
    return;
  }

  auto *epd_renderer = static_cast<EpdiyFrameBufferRenderer *>(renderer);
  if (!epd_renderer)
  {
    return;
  }

  g_paper_s3_ft_font = std::make_unique<FreeTypeFont>();
  // Use a fixed pixel height similar to the original bitmap fonts.
  int pixel_height = 22;
  if (!g_paper_s3_ft_font->init("/fs/fonts/reader.ttf", pixel_height))
  {
    g_paper_s3_ft_font.reset();
    return;
  }

  epd_renderer->set_freetype_font_for_reading(g_paper_s3_ft_font.get());
  epd_renderer->set_freetype_enabled(true);
}
#endif // BOARD_TYPE_PAPER_S3
#endif // USE_FREETYPE

#ifdef LOG_ENABLED
// Reference: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/log.html
#define LOG_LEVEL ESP_LOG_INFO
#else
#define LOG_LEVEL ESP_LOG_NONE
#endif
#include <esp_log.h>

extern "C"
{
  void app_main();
}

const char *TAG = "main";

enum class UIState
{
  SELECTING_EPUB,
  SELECTING_TABLE_CONTENTS,
  READING_EPUB,
  READING_MENU
};

// Default UI state: show the EPUB list on startup. We no longer store this in
// RTC slow memory; EPUB metadata and selection are persisted via /fs/books_index.bin.
UIState ui_state = UIState::SELECTING_EPUB;
// State data for the EPUB list and reader.
EpubListState epub_list_state = {};
// State data for the EPUB index list.
EpubTocState epub_index_state = {};

// Whether the KOReader-style status bar (title, progress, battery)
// is currently visible. Toggled via a long-press gesture while
// reading.
bool status_bar_visible = true;

// User-configurable behaviour flags.
bool open_last_book_on_startup = true;
bool invert_tap_zones = false;
bool justify_paragraphs = false;

constexpr int READER_MENU_BASIC_ITEMS = 6;
constexpr int READER_MENU_ADVANCED_ITEMS = 10;

enum class SleepImageMode
{
  COVER,
  RANDOM,
  OFF
};

SleepImageMode sleep_image_mode = SleepImageMode::COVER;

enum class IdleProfile
{
  SHORT = 0,
  NORMAL = 1,
  LONG = 2
};

enum class MarginProfile
{
  NARROW = 0,
  NORMAL = 1,
  WIDE = 2
};

enum class GestureSensitivity
{
  LOW = 0,
  MEDIUM = 1,
  HIGH = 2
};

IdleProfile idle_profile = IdleProfile::NORMAL;
MarginProfile margin_profile = MarginProfile::NORMAL;
GestureSensitivity gesture_sensitivity = GestureSensitivity::MEDIUM;

int64_t idle_timeout_reading_us = 60 * 1000 * 1000;
int64_t idle_timeout_library_us = 60 * 1000 * 1000;

typedef struct
{
  uint8_t version;
  uint8_t flags;
  uint8_t sleep_mode;
  uint8_t reserved;
#ifdef USE_FREETYPE
  int16_t reading_font_px;
  int16_t padding;
#endif
} AppSettings;

static const char *app_settings_path = "/fs/settings.bin";

static void load_app_settings(Renderer *renderer);
static void save_app_settings(Renderer *renderer);
static void apply_idle_profile();
static void apply_page_margins(Renderer *renderer);
static void apply_gesture_profile();
static void show_library_loading(Renderer *renderer);

void handleEpub(Renderer *renderer, UIAction action);
void handleEpubList(Renderer *renderer, UIAction action, bool needs_redraw);
void handleReaderMenu(Renderer *renderer, UIAction action);
static void renderReaderMenu(Renderer *renderer);
static void show_status_bar_toast(Renderer *renderer, const char *text);

static std::unique_ptr<EpubList> epub_list;
static std::unique_ptr<EpubReader> reader;
static std::unique_ptr<EpubToc> contents;
int reader_menu_selected = 0;
bool reader_menu_advanced = false;
static bool g_request_sleep_now = false;

static int find_last_open_book_index()
{
  int last_index = -1;
  auto items = std::span(epub_list_state.epub_list, epub_list_state.num_epubs);
  
  for (auto const [i, item] : std::views::enumerate(items))
  {
    if (item.current_section != 0 || item.current_page != 0)
    {
      if (last_index < 0)
      {
        last_index = i;
      }
      else
      {
        EpubListItem &best = epub_list_state.epub_list[last_index];
        if (item.current_section > best.current_section ||
            (item.current_section == best.current_section && item.current_page > best.current_page))
        {
          last_index = i;
        }
      }
    }
  }

  if (last_index >= 0)
  {
    return last_index;
  }

  // Fallback: candidate matching
  for (auto const [i, item] : std::views::enumerate(items))
  {
    if (item.pages_in_current_section > 0)
    {
      return i;
    }
  }

  return -1;
}

void handleEpub(Renderer *renderer, UIAction action)
{
  if (!reader)
  {
    reader = std::unique_ptr<EpubReader>(new EpubReader(epub_list_state.epub_list[epub_list_state.selected_item], renderer));
    reader->set_justified(justify_paragraphs);
    reader->load();
  }
  switch (action)
  {
  case UP:
    reader->prev();
    break;
  case DOWN:
    reader->next();
    break;
  case PREV_SECTION:
    reader->prev_section();
    break;
  case NEXT_SECTION:
    reader->next_section();
    break;
  case REFRESH_PAGE:
    // Force a full-screen refresh of the current reading page to
    // mitigate ghosting. This mirrors the "[R] Refresh screen"
    // reader-menu action but is triggered via a gesture.
    renderer->reset();
    break;
  case SELECT:
    // switch back to main screen
    ui_state = UIState::SELECTING_EPUB;
    renderer->clear_screen();
    // clear the epub reader away
    reader.reset();
    // force a redraw
    if (!epub_list)
    {
      epub_list = std::unique_ptr<EpubList>(new EpubList(renderer, epub_list_state));
    }
    handleEpubList(renderer, NONE, true);
    return;
  case NONE:
  default:
    break;
  }
  reader->render();
}

void handleEpubTableContents(Renderer *renderer, UIAction action, bool needs_redraw)
{
  if (!contents)
  {
    contents = std::unique_ptr<EpubToc>(new EpubToc(epub_list_state.epub_list[epub_list_state.selected_item], epub_index_state, renderer));
    contents->set_needs_redraw();
    contents->load();
  }
  switch (action)
  {
  case UP:
    contents->prev();
    break;
  case DOWN:
    contents->next();
    break;
  case SELECT:
    // setup the reader state
    ui_state = UIState::READING_EPUB;
    // Replace any existing reader so we don't leak or reuse stale
    // parser state when jumping via the TOC.
    if (reader)
    {
      reader.reset();
    }
    reader = std::unique_ptr<EpubReader>(new EpubReader(epub_list_state.epub_list[epub_list_state.selected_item], renderer));
    reader->set_justified(justify_paragraphs);
    reader->set_state_section(contents->get_selected_toc());
    if (!reader->load())
    {
      ESP_LOGE(TAG, "Failed to load EPUB when opening from TOC selection");
      // Stay in the TOC view; the user can back out to the library or
      // try another entry.
      reader.reset();
      ui_state = UIState::SELECTING_TABLE_CONTENTS;
      return;
    }
    // switch to reading the epub
    contents.reset();
    handleEpub(renderer, NONE);
    return;
  case NONE:
  default:
    break;
  }
  contents->render();
}

static void renderReaderMenu(Renderer *renderer)
{
  const int max_items = 13;
  const char *labels[max_items];
  int items_total = 0;

  char buf_status[32];
  char buf_view[32];
  char buf_startup[40];
  char buf_sleep[40];
  char buf_font[32];
  char buf_align[32];
  char buf_tap[32];
  char buf_idle[32];
  char buf_margin[32];
  char buf_gest[32];

  if (!reader_menu_advanced)
  {
    items_total = READER_MENU_BASIC_ITEMS;
    labels[0] = "Return to book";
    labels[1] = "Table of contents";
    labels[2] = "Back to library";
    labels[3] = "More";
    // Use ASCII-friendly "icon" prefixes so they render on limited fonts.
    labels[4] = "[R] Refresh screen";
    labels[5] = "[Zz] Sleep";
  }
  else
  {
    items_total = READER_MENU_ADVANCED_ITEMS;

    snprintf(buf_status, sizeof(buf_status), "Status bar: %s", status_bar_visible ? "ON" : "OFF");
    labels[0] = buf_status;

    snprintf(buf_view, sizeof(buf_view), "Library view: %s", epub_list_state.use_grid_view ? "Grid" : "List");
    labels[1] = buf_view;

    snprintf(buf_startup, sizeof(buf_startup), "Startup: %s", open_last_book_on_startup ? "Last book" : "Library");
    labels[2] = buf_startup;

    const char *sleep_mode_str = "Cover";
    if (sleep_image_mode == SleepImageMode::RANDOM)
    {
      sleep_mode_str = "Random";
    }
    else if (sleep_image_mode == SleepImageMode::OFF)
    {
      sleep_mode_str = "Off";
    }
    snprintf(buf_sleep, sizeof(buf_sleep), "Sleep image: %s", sleep_mode_str);
    labels[3] = buf_sleep;

#ifdef USE_FREETYPE
    int px = renderer->get_reading_font_pixel_height();
    const char *font_label = "Medium";
    if (px <= 18)
    {
      font_label = "Small";
    }
    else if (px >= 26)
    {
      font_label = "Large";
    }
    snprintf(buf_font, sizeof(buf_font), "Font size: %s", font_label);
#else
    snprintf(buf_font, sizeof(buf_font), "Font size");
#endif
    labels[4] = buf_font;

    snprintf(buf_align, sizeof(buf_align), "Alignment: %s", justify_paragraphs ? "Justified" : "Left");
    labels[5] = buf_align;

    snprintf(buf_tap, sizeof(buf_tap), "Tap zones: %s", invert_tap_zones ? "Inverted" : "Normal");
    labels[6] = buf_tap;

    const char *idle_str = "Normal";
    if (idle_profile == IdleProfile::SHORT)
    {
      idle_str = "Short";
    }
    else if (idle_profile == IdleProfile::LONG)
    {
      idle_str = "Long";
    }
    snprintf(buf_idle, sizeof(buf_idle), "Idle: %s", idle_str);
    labels[7] = buf_idle;

    const char *margin_str = "Normal";
    if (margin_profile == MarginProfile::NARROW)
    {
      margin_str = "Narrow";
    }
    else if (margin_profile == MarginProfile::WIDE)
    {
      margin_str = "Wide";
    }
    snprintf(buf_margin, sizeof(buf_margin), "Margins: %s", margin_str);
    labels[8] = buf_margin;

    const char *gest_str = "Medium";
    if (gesture_sensitivity == GestureSensitivity::LOW)
    {
      gest_str = "Low";
    }
    else if (gesture_sensitivity == GestureSensitivity::HIGH)
    {
      gest_str = "High";
    }
    snprintf(buf_gest, sizeof(buf_gest), "Gestures: %s", gest_str);
    labels[9] = buf_gest;
  }

#ifdef USE_FREETYPE
  renderer->set_freetype_enabled(false);
#endif

  renderer->clear_screen();
  int page_width = renderer->get_page_width();
  int page_height = renderer->get_page_height();
  int line_height = renderer->get_line_height();
  if (line_height <= 0)
  {
    line_height = 20;
  }
  if (page_height <= 0)
  {
    page_height = line_height * items_total * 2;
  }

  if (page_width <= 0)
  {
    page_width = 400;
  }

  if (items_total <= 0)
  {
#ifdef USE_FREETYPE
    renderer->set_freetype_enabled(true);
#endif
    return;
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

  int bottom_bar_height = EPUB_LIST_BOTTOM_BAR_HEIGHT;
  int content_height = page_height;
  if (bottom_bar_height > 0 && bottom_bar_height < page_height)
  {
    content_height = page_height - bottom_bar_height;
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

  int max_label_width = 0;
  for (int i = 0; i < items_total; i++)
  {
    int w = renderer->get_text_width(labels[i], false, false);
    if (w > max_label_width)
    {
      max_label_width = w;
    }
  }
  int horizontal_padding = 30;
  int button_width = max_label_width + horizontal_padding * 2;
  if (button_width > page_width - 40)
  {
    button_width = page_width - 40;
  }

  int container_width = button_width;
  int container_height = visible_count * button_height + (visible_count - 1) * button_spacing;
  int container_x = (page_width - container_width) / 2;
  if (container_x < 0)
  {
    container_x = 0;
  }
  int container_y = (content_height - container_height) / 2;
  if (container_y < 0)
  {
    container_y = 0;
  }

  int ypos = container_y;
  for (int i = 0; i < visible_count; i++)
  {
    int item_index = start_index + i;
    const char *label = labels[item_index];

    renderer->fill_rect(container_x, ypos, container_width, button_height, 255);
    renderer->draw_rect(container_x, ypos, container_width, button_height, 0);

    if (item_index == reader_menu_selected)
    {
      for (int line = 0; line < 3; line++)
      {
        renderer->draw_rect(
            container_x + line,
            ypos + line,
            container_width - 2 * line,
            button_height - 2 * line,
            0);
      }
    }

    int label_width = renderer->get_text_width(label, false, false);
    if (label_width < 0)
    {
      label_width = 0;
    }
    int text_x = container_x + (container_width - label_width) / 2;
    int center_y = ypos + (button_height / 2);
    int text_y = center_y - (3 * line_height) / 4;
    renderer->draw_text(text_x, text_y, label, false, false);

    ypos += button_height + button_spacing;
  }

  if (bottom_bar_height > 0 && bottom_bar_height <= page_height)
  {
    int bar_y = page_height - bottom_bar_height;
    renderer->fill_rect(0, bar_y, page_width, bottom_bar_height, 255);
    // int center_x = page_width / 2;
    int center_y = bar_y + bottom_bar_height / 2;

    int page_display = current_page + 1;
    if (page_display < 1)
    {
      page_display = 1;
    }
    if (page_display > total_pages)
    {
      page_display = total_pages;
    }

    const char *left_double = "<<";
    const char *left_single = "<";
    const char *right_single = ">";
    const char *right_double = ">>";

    char center[32];
    snprintf(center, sizeof(center), "%d / %d", page_display, total_pages);

    int w_ld = renderer->get_text_width(left_double, true, false);
    int w_ls = renderer->get_text_width(left_single, true, false);
    int w_center = renderer->get_text_width(center, false, false);
    int w_rs = renderer->get_text_width(right_single, true, false);
    int w_rd = renderer->get_text_width(right_double, true, false);
    if (w_ld < 0) w_ld = 0;
    if (w_ls < 0) w_ls = 0;
    if (w_center < 0) w_center = 0;
    if (w_rs < 0) w_rs = 0;
    if (w_rd < 0) w_rd = 0;

    int line_h = renderer->get_line_height();
    if (line_h <= 0)
    {
      line_h = 20;
    }
    int label_y = center_y - (3 * line_h) / 4;

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

    int bar_top = bar_y;
    int bar_bottom = bar_y + bottom_bar_height;
    int bar_height = bar_bottom - bar_top;
    if (bar_height < renderer->get_line_height() + 4)
    {
      bar_height = renderer->get_line_height() + 4;
    }

    int box_y = bar_top + 2;
    int box_h = bar_height - 4;
    if (box_h <= 0)
    {
      box_h = bar_height;
    }

    renderer->draw_rect(ld_zone_start, box_y, ld_zone_end - ld_zone_start, box_h, 0);
    renderer->draw_rect(ls_zone_start, box_y, ls_zone_end - ls_zone_start, box_h, 0);
    renderer->draw_rect(center_zone_start, box_y, center_zone_end - center_zone_start, box_h, 0);
    renderer->draw_rect(rs_zone_start, box_y, rs_zone_end - rs_zone_start, box_h, 0);
    renderer->draw_rect(rd_zone_start, box_y, rd_zone_end - rd_zone_start, box_h, 0);

    auto center_label_x = [](int zone_start, int zone_end, int text_width) {
      int w = zone_end - zone_start;
      int x = zone_start + (w - text_width) / 2;
      if (x < zone_start)
      {
        x = zone_start;
      }
      return x;
    };

    int x_ld = center_label_x(ld_zone_start, ld_zone_end, w_ld);
    int x_ls = center_label_x(ls_zone_start, ls_zone_end, w_ls);
    int x_center = center_label_x(center_zone_start, center_zone_end, w_center);
    int x_rs = center_label_x(rs_zone_start, rs_zone_end, w_rs);
    int x_rd = center_label_x(rd_zone_start, rd_zone_end, w_rd);

    renderer->draw_text(x_ld, label_y, left_double, true, false);
    renderer->draw_text(x_ls, label_y, left_single, true, false);
    renderer->draw_text(x_center, label_y, center, false, false);
    renderer->draw_text(x_rs, label_y, right_single, true, false);
    renderer->draw_text(x_rd, label_y, right_double, true, false);
  }

#ifdef USE_FREETYPE
  renderer->set_freetype_enabled(true);
#endif
}

static void show_status_bar_toast(Renderer *renderer, const char *text)
{
  if (!text)
  {
    return;
  }

  int page_width = renderer->get_page_width();
  int page_height = renderer->get_page_height();
  int line_height = renderer->get_line_height();
  if (page_width <= 0 || page_height <= 0 || line_height <= 0)
  {
    return;
  }

  int padding = 4;
  int box_height = line_height + padding * 2;
  int y = page_height - box_height - 2;
  if (y < 0)
  {
    y = 0;
  }

  // Clear a small strip at the bottom and draw the toast text.
  renderer->fill_rect(0, y, page_width, box_height, 255);
  renderer->draw_rect(0, y, page_width, box_height, 0);
  renderer->draw_text(5, y + padding + line_height / 2, text, false, false);
}

static void load_app_settings(Renderer *renderer)
{
  FILE *fp = fopen(app_settings_path, "rb");
  if (!fp)
  {
    return;
  }
  AppSettings s = {};
  if (fread(&s, sizeof(s), 1, fp) != 1)
  {
    fclose(fp);
    return;
  }
  fclose(fp);
  if (s.version != 1)
  {
    return;
  }
  status_bar_visible = (s.flags & 0x1) != 0;
  epub_list_state.use_grid_view = (s.flags & 0x2) != 0;
  open_last_book_on_startup = (s.flags & 0x4) != 0;
  invert_tap_zones = (s.flags & 0x8) != 0;
  uint8_t margin_bits = (s.flags >> 4) & 0x3;
  if (margin_bits <= static_cast<uint8_t>(MarginProfile::WIDE))
  {
    margin_profile = static_cast<MarginProfile>(margin_bits);
  }
  uint8_t idle_bits = (s.flags >> 6) & 0x3;
  if (idle_bits <= static_cast<uint8_t>(IdleProfile::LONG))
  {
    idle_profile = static_cast<IdleProfile>(idle_bits);
  }
  if (s.sleep_mode <= static_cast<uint8_t>(SleepImageMode::OFF))
  {
    sleep_image_mode = static_cast<SleepImageMode>(s.sleep_mode);
  }
#ifdef USE_FREETYPE
  if (s.reading_font_px > 0)
  {
    renderer->set_reading_font_pixel_height(s.reading_font_px);
  }
#endif
  gesture_sensitivity = static_cast<GestureSensitivity>(s.reserved & 0x3);
  // Bit 2 of reserved stores the paragraph alignment preference.
  justify_paragraphs = (s.reserved & 0x4) != 0;
  apply_idle_profile();
  apply_page_margins(renderer);
  apply_gesture_profile();
}

static void save_app_settings(Renderer *renderer)
{
  AppSettings s = {};
  s.version = 1;
  if (status_bar_visible)
  {
    s.flags |= 0x1;
  }
  if (epub_list_state.use_grid_view)
  {
    s.flags |= 0x2;
  }
  if (open_last_book_on_startup)
  {
    s.flags |= 0x4;
  }
   if (invert_tap_zones)
  {
    s.flags |= 0x8;
  }
  s.flags |= (static_cast<uint8_t>(margin_profile) & 0x3) << 4;
  s.flags |= (static_cast<uint8_t>(idle_profile) & 0x3) << 6;
  s.sleep_mode = static_cast<uint8_t>(sleep_image_mode);
#ifdef USE_FREETYPE
  int px = renderer->get_reading_font_pixel_height();
  if (px > 0)
  {
    s.reading_font_px = (int16_t)px;
  }
#endif
  s.reserved = static_cast<uint8_t>(gesture_sensitivity);
  if (justify_paragraphs)
  {
    s.reserved |= 0x4;
  }
  FILE *fp = fopen(app_settings_path, "wb");
  if (!fp)
  {
    return;
  }
  fwrite(&s, sizeof(s), 1, fp);
  fclose(fp);
}

static void apply_idle_profile()
{
  switch (idle_profile)
  {
  case IdleProfile::SHORT:
    idle_timeout_reading_us = 10 * 60 * 1000 * 1000;
    idle_timeout_library_us = 2 * 60 * 1000 * 1000;
    break;
  case IdleProfile::LONG:
    idle_timeout_reading_us = 40LL * 60 * 1000 * 1000;
    idle_timeout_library_us = 10LL * 60 * 1000 * 1000;
    break;
  case IdleProfile::NORMAL:
  default:
    idle_timeout_reading_us = 20 * 60 * 1000 * 1000;
    idle_timeout_library_us = 5 * 60 * 1000 * 1000;
    break;
  }
}

static void apply_page_margins(Renderer *renderer)
{
  int left = 10;
  int right = 10;
  switch (margin_profile)
  {
  case MarginProfile::NARROW:
    left = 5;
    right = 5;
    break;
  case MarginProfile::WIDE:
    left = 20;
    right = 20;
    break;
  case MarginProfile::NORMAL:
  default:
    left = 10;
    right = 10;
    break;
  }
  renderer->set_margin_top(35);
  renderer->set_margin_left(left);
  renderer->set_margin_right(right);
}

static void apply_gesture_profile()
{
  int profile = 1;
  switch (gesture_sensitivity)
  {
  case GestureSensitivity::LOW:
    profile = 0;
    break;
  case GestureSensitivity::HIGH:
    profile = 2;
    break;
  case GestureSensitivity::MEDIUM:
  default:
    profile = 1;
    break;
  }
  PaperS3TouchControls::set_gesture_profile(profile);
}

void handleReaderMenu(Renderer *renderer, UIAction action)
{
  int item_total = reader_menu_advanced ? READER_MENU_ADVANCED_ITEMS : READER_MENU_BASIC_ITEMS;

  switch (action)
  {
  case UP:
    if (item_total > 0)
    {
      reader_menu_selected = (reader_menu_selected - 1 + item_total) % item_total;
    }
    renderReaderMenu(renderer);
    break;
  case DOWN:
    if (item_total > 0)
    {
      reader_menu_selected = (reader_menu_selected + 1) % item_total;
    }
    renderReaderMenu(renderer);
    break;
  case SELECT:
    if (!reader_menu_advanced)
    {
      if (reader_menu_selected == 0)
      {
        ui_state = UIState::READING_EPUB;
        renderer->clear_screen();
        if (reader)
        {
          reader->render();
        }
      }
      else if (reader_menu_selected == 1)
      {
        ui_state = UIState::SELECTING_TABLE_CONTENTS;
        if (contents)
        {
          contents.reset();
        }
        contents = std::unique_ptr<EpubToc>(new EpubToc(epub_list_state.epub_list[epub_list_state.selected_item], epub_index_state, renderer));
        if (!contents->load())
        {
          contents.reset();
          ui_state = UIState::READING_EPUB;
          renderer->clear_screen();
          if (reader)
          {
            reader->render();
          }
          break;
        }
        contents->set_needs_redraw();
        handleEpubTableContents(renderer, NONE, true);
      }
      else if (reader_menu_selected == 2)
      {
        // Back to library: force a full-screen refresh and show the
        // same "Book library is loading" splash used on cold boot
        // while the EPUB list is (re)rendered.
        ui_state = UIState::SELECTING_EPUB;
        renderer->reset();
        show_library_loading(renderer);
        if (reader)
        {
          reader.reset();
        }
        handleEpubList(renderer, NONE, true);
      }
      else if (reader_menu_selected == 3)
      {
        reader_menu_advanced = true;
        reader_menu_selected = 0;
        renderReaderMenu(renderer);
      }
      else if (reader_menu_selected == 4)
      {
        // Full screen refresh of the current reading page to
        // mitigate ghosting.
        ui_state = UIState::READING_EPUB;
        renderer->reset();
        if (reader)
        {
          reader->render();
        }
      }
      else if (reader_menu_selected == 5)
      {
        // Request immediate sleep; main_task's event loop will
        // see this flag and break out to the sleep sequence.
        g_request_sleep_now = true;
      }
    }
    else
    {
      // Advanced menu entries
      if (reader_menu_selected == 0)
      {
        status_bar_visible = !status_bar_visible;
        save_app_settings(renderer);
        show_status_bar_toast(renderer, status_bar_visible ? "Status bar ON" : "Status bar OFF");
        renderReaderMenu(renderer);
      }
      else if (reader_menu_selected == 1)
      {
        epub_list_state.use_grid_view = !epub_list_state.use_grid_view;
        if (epub_list)
        {
          epub_list->set_needs_redraw();
        }
        save_app_settings(renderer);
        show_status_bar_toast(renderer, epub_list_state.use_grid_view ? "Library view: Grid" : "Library view: List");
        renderReaderMenu(renderer);
      }
      else if (reader_menu_selected == 2)
      {
        open_last_book_on_startup = !open_last_book_on_startup;
        save_app_settings(renderer);
        show_status_bar_toast(renderer, open_last_book_on_startup ? "Startup: Open last book" : "Startup: Library");
        renderReaderMenu(renderer);
      }
      else if (reader_menu_selected == 3)
      {
        switch (sleep_image_mode)
        {
        case SleepImageMode::COVER:
          sleep_image_mode = SleepImageMode::RANDOM;
          show_status_bar_toast(renderer, "Sleep image: Random");
          break;
        case SleepImageMode::RANDOM:
          sleep_image_mode = SleepImageMode::OFF;
          show_status_bar_toast(renderer, "Sleep image: Off");
          break;
        case SleepImageMode::OFF:
        default:
          sleep_image_mode = SleepImageMode::COVER;
          show_status_bar_toast(renderer, "Sleep image: Cover");
          break;
        }
        save_app_settings(renderer);
        renderReaderMenu(renderer);
      }
      else if (reader_menu_selected == 4)
      {
#ifdef USE_FREETYPE
        int sizes[] = {18, 22, 26};
        int current_px = renderer->get_reading_font_pixel_height();
        int index = 0;
        for (int i = 0; i < 3; i++)
        {
          if (sizes[i] == current_px)
          {
            index = i;
          }
        }
        index = (index + 1) % 3;
        int next_px = sizes[index];
        renderer->set_reading_font_pixel_height(next_px);
        save_app_settings(renderer);
        show_status_bar_toast(renderer, "Font size changed");
#else
        (void)renderer;
#endif
        renderReaderMenu(renderer);
      }
      else if (reader_menu_selected == 5)
      {
        // Toggle paragraph alignment between left-aligned and
        // fully-justified. The actual layout is handled by
        // RubbishHtmlParser via EpubReader::set_justified().
        justify_paragraphs = !justify_paragraphs;
        save_app_settings(renderer);
        if (reader)
        {
          reader->set_justified(justify_paragraphs);
        }
        show_status_bar_toast(renderer, justify_paragraphs ? "Alignment: Justified" : "Alignment: Left");
        renderReaderMenu(renderer);
      }
      else if (reader_menu_selected == 6)
      {
        invert_tap_zones = !invert_tap_zones;
        save_app_settings(renderer);
        show_status_bar_toast(renderer, invert_tap_zones ? "Tap zones: inverted" : "Tap zones: normal");
        renderReaderMenu(renderer);
      }
      else if (reader_menu_selected == 7)
      {
        idle_profile = static_cast<IdleProfile>(((int)idle_profile + 1) % 3);
        apply_idle_profile();
        save_app_settings(renderer);
        const char *label = "Idle: Normal";
        if (idle_profile == IdleProfile::SHORT)
        {
          label = "Idle: Short";
        }
        else if (idle_profile == IdleProfile::LONG)
        {
          label = "Idle: Long";
        }
        show_status_bar_toast(renderer, label);
        renderReaderMenu(renderer);
      }
      else if (reader_menu_selected == 8)
      {
        margin_profile = static_cast<MarginProfile>(((int)margin_profile + 1) % 3);
        apply_page_margins(renderer);
        save_app_settings(renderer);
        const char *label = "Margins: Normal";
        if (margin_profile == MarginProfile::NARROW)
        {
          label = "Margins: Narrow";
        }
        else if (margin_profile == MarginProfile::WIDE)
        {
          label = "Margins: Wide";
        }
        show_status_bar_toast(renderer, label);
        renderReaderMenu(renderer);
      }
      else if (reader_menu_selected == 9)
      {
        gesture_sensitivity = static_cast<GestureSensitivity>(((int)gesture_sensitivity + 1) % 3);
        apply_gesture_profile();
        save_app_settings(renderer);
        const char *label = "Gestures: Medium";
        if (gesture_sensitivity == GestureSensitivity::LOW)
        {
          label = "Gestures: Low";
        }
        else if (gesture_sensitivity == GestureSensitivity::HIGH)
        {
          label = "Gestures: High";
        }
        show_status_bar_toast(renderer, label);
        renderReaderMenu(renderer);
      }
    }
    break;
  case NONE:
  default:
    renderReaderMenu(renderer);
    break;
  }
}

void handleEpubList(Renderer *renderer, UIAction action, bool needs_redraw)
{
  // load up the epub list from the filesystem
  if (!epub_list)
  {
    ESP_LOGI("main", "Creating epub list");
    epub_list = std::unique_ptr<EpubList>(new EpubList(renderer, epub_list_state));
    // Paper S3 stores all EPUBs on the SD card under /fs/Books.
    if (epub_list->load("/fs/Books"))
    {
      ESP_LOGI("main", "Epub files loaded");
    }
  }
  if (needs_redraw)
  {
    epub_list->set_needs_redraw();
  }
  // work out what the user wants us to do
  switch (action)
  {
  case UP:
    epub_list->prev();
    break;
  case DOWN:
    epub_list->next();
    break;
  case SELECT:
    // Try to show the table of contents if the book has one; otherwise
    // fall back to opening the book directly.
    ui_state = UIState::SELECTING_TABLE_CONTENTS;
    contents = std::unique_ptr<EpubToc>(new EpubToc(epub_list_state.epub_list[epub_list_state.selected_item], epub_index_state, renderer));
    if (!contents->load())
    {
      contents.reset();
      ui_state = UIState::READING_EPUB;
      handleEpub(renderer, NONE);
      return;
    }
    contents->set_needs_redraw();
    handleEpubTableContents(renderer, NONE, true);
    return;
  case NONE:
  default:
    // nothing to do
    break;
  }
  epub_list->render();
}

void handleUserInteraction(Renderer *renderer, UIAction ui_action, bool needs_redraw)
{
  // Global handling for status bar toggle while reading.
  if (ui_action == TOGGLE_STATUS_BAR && ui_state == UIState::READING_EPUB)
  {
    status_bar_visible = !status_bar_visible;
    save_app_settings(renderer);
    // Re-render the current page; draw_battery_level() will
    // pick up the new visibility on the next flush.
    handleEpub(renderer, NONE);
    show_status_bar_toast(renderer, status_bar_visible ? "Status bar ON" : "Status bar OFF");
    return;
  }

  // From the library view, allow a gesture (e.g. two-finger swipe up)
  // to open the reader menu directly, focusing on advanced settings.
  if (ui_action == OPEN_READER_MENU && ui_state == UIState::SELECTING_EPUB)
  {
    ui_state = UIState::READING_MENU;
    reader_menu_advanced = true;
    reader_menu_selected = 0;
    renderReaderMenu(renderer);
    return;
  }

  switch (ui_state)
  {
  case UIState::READING_MENU:
    handleReaderMenu(renderer, ui_action);
    break;
  case UIState::READING_EPUB:
    if (ui_action == SELECT)
    {
      ui_state = UIState::READING_MENU;
      reader_menu_selected = 0;
      renderReaderMenu(renderer);
    }
    else
    {
      handleEpub(renderer, ui_action);
    }
    break;
  case UIState::SELECTING_TABLE_CONTENTS:
    handleEpubTableContents(renderer, ui_action, needs_redraw);
    break;
  case UIState::SELECTING_EPUB:
  default:
    handleEpubList(renderer, ui_action, needs_redraw);
    break;
  }
}

void draw_battery_level(Renderer *renderer, float voltage, float percentage)
{
  // If the status bar is hidden, restore full-page content by
  // removing the reserved top margin and skip drawing any
  // status elements.
  if (!status_bar_visible)
  {
    renderer->set_margin_top(0);
    return;
  }

  // clear the margin so we can draw the battery in the right place
  renderer->set_margin_top(0);
  int width = 40;
  int height = 20;
  int margin_right = 5;
  int margin_top = 10;
  int xpos = renderer->get_page_width() - width - margin_right;
  int ypos = margin_top;
  int percent_width = width * percentage / 100;
  renderer->fill_rect(xpos, ypos, width, height, 255);
  renderer->fill_rect(xpos + width - percent_width, ypos, percent_width, height, 0);
  renderer->draw_rect(xpos, ypos, width, height, 0);
  renderer->fill_rect(xpos - 4, ypos + height / 4, 4, height / 2, 0);
  // put the margin back
  renderer->set_margin_top(35);
}

static void show_library_loading(Renderer *renderer)
{
  renderer->clear_screen();
  int page_width = renderer->get_page_width();
  int page_height = renderer->get_page_height();
  int line_height = renderer->get_line_height();
  if (page_width <= 0 || page_height <= 0 || line_height <= 0)
  {
    return;
  }

  const char *msg = "Book library is loading";
  int text_width = renderer->get_text_width(msg, false, false);
  if (text_width < 0)
  {
    text_width = 0;
  }

  int x = (page_width - text_width) / 2;
  if (x < 0)
  {
    x = 0;
  }
  int center_y = page_height / 2;
  int y = center_y - (3 * line_height) / 4;

  renderer->draw_text(x, y, msg, false, false);
  renderer->flush_display();
}

static void show_sleep_cover(Renderer *renderer)
{
  int book_index = -1;
  if (epub_list_state.num_epubs > 0)
  {
    if (ui_state == UIState::READING_EPUB || ui_state == UIState::READING_MENU || ui_state == UIState::SELECTING_TABLE_CONTENTS)
    {
      book_index = epub_list_state.selected_item;
    }
    if (book_index < 0)
    {
      book_index = find_last_open_book_index();
    }
  }

  if (book_index < 0)
  {
    return;
  }

  EpubListItem &item = epub_list_state.epub_list[book_index];
  if (item.cover_path[0] == '\0')
  {
    return;
  }

  Epub epub(item.path);
  if (!epub.load())
  {
    return;
  }

  size_t image_data_size = 0;
  uint8_t *image_data = epub.get_item_contents(item.cover_path, &image_data_size);
  if (!image_data || image_data_size == 0)
  {
    free(image_data);
    return;
  }

  int img_w = 0;
  int img_h = 0;
  bool can_render = renderer->get_image_size(item.cover_path, image_data, image_data_size, &img_w, &img_h);
  if (!can_render || img_w <= 0 || img_h <= 0)
  {
    free(image_data);
    return;
  }

  renderer->set_margin_top(0);
  renderer->set_margin_bottom(0);
  renderer->set_margin_left(0);
  renderer->set_margin_right(0);

  int width = renderer->get_page_width();
  int height = renderer->get_page_height();

  renderer->clear_screen();
  renderer->set_image_placeholder_enabled(false);
  renderer->draw_image(item.cover_path, image_data, image_data_size, 0, 0, width, height);
  renderer->set_image_placeholder_enabled(true);
  free(image_data);
  renderer->flush_display();
}

static void show_sleep_image(Renderer *renderer)
{
  if (sleep_image_mode == SleepImageMode::OFF)
  {
    return;
  }

  if (sleep_image_mode == SleepImageMode::COVER)
  {
    show_sleep_cover(renderer);
    return;
  }

  if (sleep_image_mode != SleepImageMode::RANDOM)
  {
    return;
  }

  const char *pics_dir = nullptr;
  DIR *dir = nullptr;
  const char *candidates[] = {"/fs/Images", "/fs/images", "/fs/Pics"};
  for (int i = 0; i < 2; i++)
  {
    dir = opendir(candidates[i]);
    if (dir)
    {
      pics_dir = candidates[i];
      break;
    }
  }
  if (!dir)
  {
    ESP_LOGW("main", "Sleep image directory not found: %s", "/fs/Images");
    show_sleep_cover(renderer);
    return;
  }

  char selected_path[512] = {0};
  int image_count = 0;
  struct dirent *ent;

  while ((ent = readdir(dir)) != nullptr)
  {
    if (ent->d_name[0] == '.' || ent->d_type == DT_DIR)
    {
      continue;
    }

    const char *dot = strrchr(ent->d_name, '.');
    if (!dot || !dot[1])
    {
      continue;
    }
    const char *ext = dot + 1;
    char e0 = tolower(ext[0]);
    char e1 = tolower(ext[1]);
    char e2 = tolower(ext[2]);
    char e3 = tolower(ext[3]);

    bool is_jpg = (e0 == 'j' && e1 == 'p' && e2 == 'g' && ext[3] == '\0');
    bool is_jpeg = (e0 == 'j' && e1 == 'p' && e2 == 'e' && e3 == 'g' && ext[4] == '\0');
    bool is_png = (e0 == 'p' && e1 == 'n' && e2 == 'g' && ext[3] == '\0');
    if (!is_jpg && !is_jpeg && !is_png)
    {
      continue;
    }

    // Reservoir sampling so each image has equal probability.
    image_count++;
    if (image_count == 1)
    {
      snprintf(selected_path, sizeof(selected_path), "%s/%s", pics_dir, ent->d_name);
    }
    else
    {
      uint32_t r = esp_random();
      if (r % (uint32_t)image_count == 0)
      {
        snprintf(selected_path, sizeof(selected_path), "%s/%s", pics_dir, ent->d_name);
      }
    }
  }
  closedir(dir);

  if (image_count == 0 || selected_path[0] == '\0')
  {
    ESP_LOGW("main", "No image files found in %s", pics_dir);
    show_sleep_cover(renderer);
    return;
  }

  const char *sleep_image_path = selected_path;
  FILE *fp = fopen(sleep_image_path, "rb");
  if (!fp)
  {
    ESP_LOGW("main", "Sleep image not found at %s", sleep_image_path);
    show_sleep_cover(renderer);
    return;
  }

  if (fseek(fp, 0, SEEK_END) != 0)
  {
    fclose(fp);
    show_sleep_cover(renderer);
    return;
  }
  long size = ftell(fp);
  if (size <= 0)
  {
    fclose(fp);
    ESP_LOGW("main", "Sleep image file has invalid size: %s", sleep_image_path);
    show_sleep_cover(renderer);
    return;
  }
  if (fseek(fp, 0, SEEK_SET) != 0)
  {
    fclose(fp);
    show_sleep_cover(renderer);
    return;
  }

  std::vector<uint8_t> data(size);
  if (data.empty())
  {
    fclose(fp);
    ESP_LOGW("main", "Failed to allocate memory for sleep image");
    show_sleep_cover(renderer);
    return;
  }

  size_t read = fread(data.data(), 1, (size_t)size, fp);
  fclose(fp);
  if (read != (size_t)size)
  {
    ESP_LOGW("main", "Failed to read full sleep image");
    show_sleep_cover(renderer);
    return;
  }

  int img_w = 0;
  int img_h = 0;
  bool can_render = renderer->get_image_size(sleep_image_path, data.data(), (size_t)size, &img_w, &img_h);
  if (!can_render || img_w <= 0 || img_h <= 0)
  {
    ESP_LOGW("main", "Sleep image decode failed: %s", sleep_image_path);
    show_sleep_cover(renderer);
    return;
  }

  // Draw full-screen, ignoring margins.
  renderer->set_margin_top(0);
  renderer->set_margin_bottom(0);
  renderer->set_margin_left(0);
  renderer->set_margin_right(0);

  int width = renderer->get_page_width();
  int height = renderer->get_page_height();

  renderer->clear_screen();
  // For sleep images we do not want to show the generic cover placeholder
  // if decoding fails; just leave the screen as-is in that case.
  renderer->set_image_placeholder_enabled(false);
  renderer->draw_image(sleep_image_path, data.data(), (size_t)size, 0, 0, width, height);
  renderer->set_image_placeholder_enabled(true);
  renderer->flush_display();
}

void main_task(void *param)
{
  // start the board up
  ESP_LOGI("main", "Powering up the board");
  Board *board = Board::factory();
  board->power_up();
  // create the renderer for the board
  ESP_LOGI("main", "Creating renderer");
  Renderer *renderer = board->get_renderer();
  // bring the file system up - SPIFFS or SDCard depending on the defines in platformio.ini
  ESP_LOGI("main", "Starting file system");
  board->start_filesystem();

#ifdef USE_FREETYPE
#if defined(BOARD_TYPE_PAPER_S3)
  // For Paper S3, initialize the global FreeType font once the filesystem
  // is available so that all subsequent UI and reading text rendering uses
  // the TTF font from /fs.
  init_freetype_for_paper_s3(renderer);
#endif
#endif

  load_app_settings(renderer);

  // battery details
  ESP_LOGI("main", "Starting battery monitor");
  Battery *battery = board->get_battery();
  if (battery)
  {
    battery->setup();
  }

  apply_page_margins(renderer);

  // create a message queue for UI events
  QueueHandle_t ui_queue = xQueueCreate(10, sizeof(UIAction));

  // set the controls up
  ESP_LOGI("main", "Setting up controls");
  ButtonControls *button_controls = board->get_button_controls(ui_queue);
  TouchControls *touch_controls = board->get_touch_controls(renderer, ui_queue);

  ESP_LOGI("main", "Controls configured");
  // work out if we were woken from deep sleep
  if (button_controls->did_wake_from_deep_sleep())
  {
    // restore the renderer state - it should have been saved when we went to sleep...
    bool hydrate_success = renderer->hydrate();
    UIAction ui_action = button_controls->get_deep_sleep_action();

#if defined(BOARD_TYPE_PAPER_S3)
    // On Paper S3, a deep-sleep wake should always behave like
    // "resume reading": rebuild the EPUB list state and jump
    // straight back into the last-open book and page, regardless
    // of the Startup preference (which only affects cold boots).
    if (!epub_list)
    {
      epub_list = std::unique_ptr<EpubList>(new EpubList(renderer, epub_list_state));
      epub_list->load("/fs/Books");
    }
    int last_book_index = find_last_open_book_index();
    if (last_book_index >= 0)
    {
      epub_list_state.selected_item = last_book_index;
      ui_state = UIState::READING_EPUB;
      // Ignore any deep-sleep button action on Paper S3 (there
      // are no navigation buttons); we just want to render the
      // last-opened page.
      ui_action = NONE;
    }
#endif

    handleUserInteraction(renderer, ui_action, !hydrate_success);
  }
  else
  {
    // reset the screen
    renderer->reset();
    show_library_loading(renderer);
    if (!epub_list)
    {
      epub_list = std::unique_ptr<EpubList>(new EpubList(renderer, epub_list_state));
      epub_list->load("/fs/Books");
    }
    if (open_last_book_on_startup)
    {
      int last_book_index = find_last_open_book_index();
      if (last_book_index >= 0)
      {
        epub_list_state.selected_item = last_book_index;
        ui_state = UIState::READING_EPUB;
      }
    }
    // make sure the UI is in the right state
    handleUserInteraction(renderer, NONE, true);
  }

  // draw the battery level before flushing the screen
  if (battery)
  {
    draw_battery_level(renderer, battery->get_voltage(), battery->get_percentage());
  }
  touch_controls->render(renderer);
  renderer->flush_display();

  // keep track of when the user last interacted and go to sleep after N seconds
  int64_t last_user_interaction = esp_timer_get_time();
  int64_t last_battery_update = last_user_interaction;
  bool screen_dirty = false;
  const int64_t battery_update_interval_us = 60 * 1000 * 1000;
  while (true)
  {
    if (g_request_sleep_now)
    {
      break;
    }

    bool in_reading_context = (ui_state == UIState::READING_EPUB || ui_state == UIState::READING_MENU || ui_state == UIState::SELECTING_TABLE_CONTENTS);
    int64_t idle_timeout_us = in_reading_context ? idle_timeout_reading_us : idle_timeout_library_us;
    if (esp_timer_get_time() - last_user_interaction >= idle_timeout_us)
    {
      break;
    }
    UIAction ui_action = NONE;
    // wait for something to happen for 60 seconds
    if (xQueueReceive(ui_queue, &ui_action, pdMS_TO_TICKS(60000)) == pdTRUE)
    {
      if (ui_action != NONE)
      {
        // something happened!
        last_user_interaction = esp_timer_get_time();
        // show feedback on the touch controls
        touch_controls->renderPressedState(renderer, ui_action);
        handleUserInteraction(renderer, ui_action, false);

        // make sure to clear the feedback on the touch controls
        touch_controls->render(renderer);
        if (battery)
        {
          draw_battery_level(renderer, battery->get_voltage(), battery->get_percentage());
        }
        screen_dirty = true;
      }
    }
    int64_t now = esp_timer_get_time();
    if (battery && (now - last_battery_update) >= battery_update_interval_us)
    {
      last_battery_update = now;
      ESP_LOGI("main", "Battery Level %f, percent %d", battery->get_voltage(), battery->get_percentage());
      draw_battery_level(renderer, battery->get_voltage(), battery->get_percentage());

      int top_width = renderer->get_page_width();
      int top_height = 50;
      if (top_width > 0 && top_height > 0)
      {
        renderer->flush_area(0, 0, top_width, top_height);
      }
    }
    if (screen_dirty)
    {
      renderer->flush_display();
      screen_dirty = false;
    }
  }
  // Persist EPUB list state (including current section/page) so that
  // cold boots and deep-sleep resumes can restore the last-read book
  // and page via the BOOKS.IDX index.
  if (epub_list)
  {
    epub_list->save_index("/fs/Books/BOOKS.IDX");
  }
  show_sleep_image(renderer);
  ESP_LOGI("main", "Saving state");
  // save the state of the renderer
  renderer->dehydrate();
  // turn off the filesystem
  board->stop_filesystem();
  // get ready to go to sleep
  board->prepare_to_sleep();
  esp_err_t err = esp_sleep_enable_ulp_wakeup();
  if (err != ESP_OK)
  {
    ESP_LOGW("main", "esp_sleep_enable_ulp_wakeup failed: %s", esp_err_to_name(err));
  }
  ESP_LOGI("main", "Entering deep sleep");
  // configure deep sleep options
  button_controls->setup_deep_sleep();
  vTaskDelay(pdMS_TO_TICKS(500));
  // go to sleep
  esp_deep_sleep_start();
}

void app_main()
{
  // Logging control
  esp_log_level_set("main", LOG_LEVEL);
  esp_log_level_set("EPUB", LOG_LEVEL);
  esp_log_level_set("PUBLIST", LOG_LEVEL);
  esp_log_level_set("ZIP", LOG_LEVEL);
  esp_log_level_set("JPG", LOG_LEVEL);
  esp_log_level_set("TOUCH", LOG_LEVEL);

  // dump out the epub list state
  ESP_LOGI("main", "epub list state num_epubs=%d", epub_list_state.num_epubs);
  ESP_LOGI("main", "epub list state is_loaded=%d", epub_list_state.is_loaded);
  ESP_LOGI("main", "epub list state selected_item=%d", epub_list_state.selected_item);

  ESP_LOGI("main", "Memory before main task start %d", esp_get_free_heap_size());
  xTaskCreatePinnedToCore(main_task, "main_task", 32768, nullptr, 1, nullptr, 1);
}