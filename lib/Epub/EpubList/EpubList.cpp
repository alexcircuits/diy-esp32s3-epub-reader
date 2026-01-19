#include "EpubList.h"

#include <ctype.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

#ifndef UNIT_TEST
  #include <freertos/FreeRTOS.h>
  #include <freertos/task.h>
  #include <esp_log.h>
#else
  #define vTaskDelay(t)
  #define ESP_LOGE(args...)
  #define ESP_LOGI(args...)
  #define ESP_LOGD(args...)
#endif

static const char *TAG = "PUBLIST";

#define PADDING 20
#define EPUBS_PER_PAGE 5

void EpubList::next()
{
  if (state.num_epubs == 0)
  {
    return;
  }
  state.selected_item = (state.selected_item + 1) % state.num_epubs;
}

void EpubList::prev()
{
  if (state.num_epubs == 0)
  {
    return;
  }
  state.selected_item = (state.selected_item - 1 + state.num_epubs) % state.num_epubs;
}

bool EpubList::load(const char *path)
{
  if (state.is_loaded)
  {
    ESP_LOGD(TAG, "Already loaded books");
    return true;
  }
  // Use an 8.3-safe filename so the FAT implementation can always create it.
  const char *index_path = "/fs/Books/BOOKS.IDX";
  if (load_index(path, index_path))
  {
    ESP_LOGI(TAG, "Loaded EPUB index from %s", index_path);
    return true;
  }
  renderer->show_busy();
  // trigger a proper redraw
  state.previous_rendered_page = -1;
  // read in the list of epubs
  state.num_epubs = 0;
  // normalise the base path for epub files so we can support
  // directories like /fs/Books as well as the root /fs/.
  std::string base_path = path;
  if (!base_path.empty() && base_path.back() != '/')
  {
    base_path += "/";
  }
  DIR *dir;
  struct dirent *ent;
  if ((dir = opendir(path)) != nullptr)
  {
    while ((ent = readdir(dir)) != nullptr)
    {
      ESP_LOGD(TAG, "Found file: %s", ent->d_name);
      // ignore any hidden files starting with "." and any directories
      // Also ignore files starting with '_' which on macOS FAT volumes are
      // typically AppleDouble metadata (e.g. "_THEGR~6.EPU").
      if (ent->d_name[0] == '.' || ent->d_name[0] == '_' || ent->d_type == DT_DIR)
      {
        continue;
      }

      const char *dot = strrchr(ent->d_name, '.');
      if (!dot || !dot[1])
      {
        continue;
      }
      const char *ext = dot + 1;
      // Accept "epub" and also the 8.3 short-name variant "epu" (case-insensitive)
      char e0 = tolower(ext[0]);
      char e1 = tolower(ext[1]);
      char e2 = tolower(ext[2]);
      char e3 = tolower(ext[3]);
      bool is_epu = (e0 == 'e' && e1 == 'p' && e2 == 'u' && (ext[3] == '\0'));
      bool is_epub = (e0 == 'e' && e1 == 'p' && e2 == 'u' && e3 == 'b' && ext[4] == '\0');
      if (!is_epu && !is_epub)
      {
        continue;
      }
      ESP_LOGD(TAG, "Loading epub %s", ent->d_name);
      Epub *epub = new Epub(base_path + ent->d_name);
      if (epub->load())
      {
        strncpy(state.epub_list[state.num_epubs].path, epub->get_path().c_str(), MAX_PATH_SIZE);
        strncpy(state.epub_list[state.num_epubs].title, replace_html_entities(epub->get_title()).c_str(), MAX_TITLE_SIZE);
        const std::string &cover_item = epub->get_cover_image_item();
        if (!cover_item.empty())
        {
          // Copy the declared cover path into the persisted state, then
          // validate that the image can actually be decoded. If anything
          // about the cover is invalid (missing resource, corrupt data,
          // or nonsensical dimensions), treat it as "no cover" so the
          // UI will render a safe title-only card instead of attempting
          // to draw a bad image later in the grid/list or sleep cover.
          strncpy(state.epub_list[state.num_epubs].cover_path, cover_item.c_str(), MAX_PATH_SIZE);
          state.epub_list[state.num_epubs].cover_path[MAX_PATH_SIZE - 1] = '\0';

          size_t cover_data_size = 0;
          uint8_t *cover_data = epub->get_item_contents(cover_item, &cover_data_size);
          bool valid_cover = false;
          if (cover_data && cover_data_size > 0)
          {
            int cw = 0;
            int ch = 0;
            if (renderer->get_image_size(cover_item, cover_data, cover_data_size, &cw, &ch) && cw > 0 && ch > 0)
            {
              // Reject covers that are implausibly large relative to the
              // device resolution. Extremely high-resolution or corrupt
              // images can cause slow, oversized rendering in the grid.
              int page_w = renderer->get_page_width();
              int page_h = renderer->get_page_height();
              int max_dim = std::max(page_w, page_h);
              if (max_dim <= 0)
              {
                max_dim = 4000; // conservative upper bound
              }
              int max_allowed = max_dim * 4; // allow up to 4x screen size
              if (cw <= max_allowed && ch <= max_allowed)
              {
                valid_cover = true;
              }
            }
          }
          if (!valid_cover)
          {
            ESP_LOGW(TAG, "Invalid cover for '%s', using title-only card instead", state.epub_list[state.num_epubs].title);
            state.epub_list[state.num_epubs].cover_path[0] = '\0';
          }
          free(cover_data);
        }
        else
        {
          state.epub_list[state.num_epubs].cover_path[0] = '\0';
        }
        state.num_epubs++;
        if (state.num_epubs == MAX_EPUB_LIST_SIZE)
        {
          ESP_LOGE(TAG, "Too many epubs, max is %d", MAX_EPUB_LIST_SIZE);
          break;
        }
      }
      else
      {
        ESP_LOGE(TAG, "Failed to load epub %s", ent->d_name);
      }
      delete epub;
    }
    closedir(dir);
    std::ranges::sort(std::span(state.epub_list, state.num_epubs),
        [](const EpubListItem &a, const EpubListItem &b)
        {
          return strcmp(a.title, b.title) < 0;
        });
  }
  else
  {
    renderer->clear_screen();
    uint16_t y = renderer->get_page_height()/2-80;
    renderer->show_img(18, y+41, warning_width, warning_height, warning_data);
    const char * warning = "Please insert SD Card";
    renderer->draw_rect(1, y, renderer->get_text_width(warning, true, false)+150, 115, 80);
    renderer->draw_text_box(warning, warning_width+25, y+4, renderer->get_page_width(), 80, true, false);
    renderer->draw_text_box("Restarting in 10 secs.", warning_width+25, y+34, renderer->get_page_width(), 80, false, false);
    perror("");
    renderer->flush_display();
    ESP_LOGE(TAG, "Is SD-Card inserted and properly connected?\nCould not open directory %s", path);
    #ifndef UNIT_TEST
      vTaskDelay(pdMS_TO_TICKS(1000*10));
      esp_restart();
    #endif
    
    return false;
  }
  // sanity check our state
  if (state.selected_item >= state.num_epubs)
  {
    state.selected_item = 0;
    state.previous_rendered_page = -1;
    state.previous_selected_item = -1;
  }
  ESP_LOGI(TAG, "Loaded %d EPUBs from %s", state.num_epubs, path);
  state.is_loaded = true;
  // prepare title layout cache
  // ensure the title blocks vector has an entry for each epub; layout will
  // be performed lazily on first render for each item.
  // (We avoid storing pointers in RTC state; this cache is per-session only.)
  // Note: MAX_EPUB_LIST_SIZE is small (20), so this is safe.
  extern EpubListState epub_list_state; // declaration elsewhere; we use state directly here
  (void)epub_list_state;
  // ensure capacity but leave pointers null for lazy init
  // m_title_blocks is a member of EpubList; here we just size it
  // to match the number of epubs.
  // Any existing blocks (if reload ever happens) are left as-is for indices < num_epubs.
  if (m_title_blocks.size() < static_cast<size_t>(state.num_epubs))
  {
    m_title_blocks.resize(state.num_epubs, nullptr);
  }
  save_index(index_path);
  return true;
}

void EpubList::render()
{
  ESP_LOGD(TAG, "Rendering EPUB list");
  int page_width = renderer->get_page_width();
  int page_height = renderer->get_page_height();
  if (page_width <= 0 || page_height <= 0)
  {
    return;
  }
  // Ensure the cache directory exists.
  ensure_cache_dir();

  int bottom_bar_height = EPUB_LIST_BOTTOM_BAR_HEIGHT;
  int content_height = page_height;
  if (bottom_bar_height > 0 && bottom_bar_height < page_height)
  {
    content_height = page_height - bottom_bar_height;
  }

  int items_per_page = state.use_grid_view ? (EPUB_GRID_ROWS * EPUB_GRID_COLUMNS) : EPUB_LIST_ITEMS_PER_PAGE;
  if (items_per_page <= 0)
  {
    items_per_page = 1;
  }

  // what page are we on?
  int current_page = 0;
  if (state.num_epubs > 0)
  {
    current_page = state.selected_item / items_per_page;
  }
  // draw a page of epubs
  ESP_LOGD(TAG, "Current page is %d, previous page %d, redraw=%d", current_page, state.previous_rendered_page, m_needs_redraw);
  // starting a fresh page or rendering from scratch?
  if (current_page != state.previous_rendered_page || m_needs_redraw)
  {
    m_needs_redraw = false;
    renderer->clear_screen();
    state.previous_selected_item = -1;
    // trigger a redraw of the items
    state.previous_rendered_page = -1;
  }

  int start_index = current_page * items_per_page;

  if (m_title_blocks.size() < static_cast<size_t>(state.num_epubs))
  {
    m_title_blocks.resize(state.num_epubs, nullptr);
  }

  if (state.use_grid_view)
  {
    int cell_width = page_width / EPUB_GRID_COLUMNS;
    int cell_height = content_height / EPUB_GRID_ROWS;
    ESP_LOGD(TAG, "Grid cell size is %dx%d", cell_width, cell_height);
    int end_index = std::min(start_index + items_per_page, state.num_epubs);
    for (int i : std::views::iota(start_index, end_index))
    {
      int index_in_page = i - start_index;
      int row = index_in_page / EPUB_GRID_COLUMNS;
      int col = index_in_page % EPUB_GRID_COLUMNS;
      int cell_x = col * cell_width;
      int cell_y = row * cell_height;

      // do we need to draw a new page of items?
      if (current_page != state.previous_rendered_page)
      {
        ESP_LOGD(TAG, "Rendering item %d", i);
        Epub epub(state.epub_list[i].path);
        int available_height = cell_height - PADDING * 2;
        if (available_height < 1)
        {
          available_height = 1;
        }
        int image_height = available_height;
        int image_width = 2 * image_height / 3;
        if (image_width > cell_width - PADDING * 2)
        {
          image_width = cell_width - PADDING * 2;
        }
        int image_xpos = cell_x + (cell_width - image_width) / 2;
        int image_ypos = cell_y + (cell_height - image_height) / 2;

        bool needs_title_card = false;
        size_t image_data_size = 0;
        uint8_t *image_data = nullptr;

        // Check if the cover image is cached.
          if (state.epub_list[i].cover_path[0] != '\0')
        {
          std::string cache_path = get_cache_path(state.epub_list[i].path);
          struct stat st;
          std::vector<uint8_t> image_buffer;
          if (stat(cache_path.c_str(), &st) == 0)
          {
             FILE *f = fopen(cache_path.c_str(), "rb");
             if (f)
             {
               fseek(f, 0, SEEK_END);
               image_data_size = ftell(f);
               fseek(f, 0, SEEK_SET);
               image_buffer.resize(image_data_size);
               image_data = image_buffer.data();
               if (image_data)
               {
                 fread(image_data, 1, image_data_size, f);
               }
               fclose(f);
             }
          }

          if (!image_data)
          {
            image_buffer = epub.get_item_contents_as_vector(state.epub_list[i].cover_path);
            image_data = image_buffer.data();
            image_data_size = image_buffer.size();
            // Cache the cover image.
            if (image_data && image_data_size > 0)
            {
               FILE *f = fopen(cache_path.c_str(), "wb");
               if (f)
               {
                 fwrite(image_data, 1, image_data_size, f);
                 fclose(f);
               }
            }
          }
          bool can_render = false;
          if (image_data && image_data_size > 0)
          {
            int dummy_w = 0;
            int dummy_h = 0;
            can_render = renderer->get_image_size(state.epub_list[i].cover_path, image_data, image_data_size, &dummy_w, &dummy_h);
          }
          if (can_render)
          {
            renderer->set_image_placeholder_enabled(false);
            renderer->draw_image(state.epub_list[i].cover_path, image_data, image_data_size, image_xpos, image_ypos, image_width, image_height);
            renderer->set_image_placeholder_enabled(true);
          }
          else
          {
            needs_title_card = true;
          }
        }
        else
        {
          needs_title_card = true;
        }

        if (needs_title_card)
        {
          // Draw a simple bordered container with the book title only.
          int card_x = image_xpos;
          int card_y = image_ypos;
          int card_w = image_width;
          int card_h = image_height;
          if (card_w > 0 && card_h > 0)
          {
            renderer->fill_rect(card_x, card_y, card_w, card_h, 255);
            renderer->draw_rect(card_x, card_y, card_w, card_h, 0);
            int inner_padding = 4;
            int inner_x = card_x + inner_padding;
            int inner_y = card_y + inner_padding;
            int inner_w = card_w - inner_padding * 2;
            int inner_h = card_h - inner_padding * 2;
            if (inner_w > 0 && inner_h > 0)
            {
              // Use bold for the title in the grid title card.
              renderer->draw_text_box(state.epub_list[i].title, inner_x, inner_y, inner_w, inner_h, true, false);
            }
          }
        }

        // image_buffer goes out of scope and frees memory
        // Yield briefly while rendering a page of covers so the main
        // task does not starve the watchdog when decoding many JPEGs.
        vTaskDelay(1);
      }

      int sel_x = cell_x + 2;
      int sel_y = cell_y + 2;
      int sel_w = cell_width - 4;
      int sel_h = cell_height - 4;
      if (sel_w < 0)
      {
        sel_w = 0;
      }
      if (sel_h < 0)
      {
        sel_h = 0;
      }

      if (state.previous_selected_item == i)
      {
        for (int line = 0; line < 3; line++)
        {
          renderer->draw_rect(sel_x + line, sel_y + line, sel_w - 2 * line, sel_h - 2 * line, 255);
        }
      }
      if (state.selected_item == i)
      {
        for (int line = 0; line < 3; line++)
        {
          renderer->draw_rect(sel_x + line, sel_y + line, sel_w - 2 * line, sel_h - 2 * line, 0);
        }
      }
    }
  }
  else
  {
    int cell_height = content_height / EPUB_LIST_ITEMS_PER_PAGE;
    ESP_LOGD(TAG, "Cell height is %d", cell_height);
    int ypos = 0;
    int end_index = std::min(start_index + EPUB_LIST_ITEMS_PER_PAGE, state.num_epubs);
    for (int i : std::views::iota(start_index, end_index))
    {
      // do we need to draw a new page of items?
      if (current_page != state.previous_rendered_page)
      {
        ESP_LOGI(TAG, "Rendering item %d", i);
        Epub epub(state.epub_list[i].path);
        // draw the cover page
        int image_xpos = PADDING;
        int image_ypos = ypos + PADDING;
        int image_height = cell_height - PADDING * 2;
        int image_width = 2 * image_height / 3;
        size_t image_data_size = 0;
        uint8_t *image_data = nullptr;

        bool needs_title_card = false;

        // Load the cover image if it exists.
        if (state.epub_list[i].cover_path[0] != '\0')
        {
          std::string cache_path = get_cache_path(state.epub_list[i].path);
          struct stat st;
          std::vector<uint8_t> image_buffer;
          if (stat(cache_path.c_str(), &st) == 0)
          {
             FILE *f = fopen(cache_path.c_str(), "rb");
             if (f)
             {
               fseek(f, 0, SEEK_END);
               image_data_size = ftell(f);
               fseek(f, 0, SEEK_SET);
               image_buffer.resize(image_data_size);
               image_data = image_buffer.data();
               if (image_data)
               {
                 fread(image_data, 1, image_data_size, f);
               }
               fclose(f);
             }
          }

          if (!image_data)
          {
            image_buffer = epub.get_item_contents_as_vector(state.epub_list[i].cover_path);
            image_data = image_buffer.data();
            image_data_size = image_buffer.size();

            if (image_data && image_data_size > 0)
            {
               FILE *f = fopen(cache_path.c_str(), "wb");
               if (f)
               {
                 fwrite(image_data, 1, image_data_size, f);
                 fclose(f);
               }
            }
          }
          bool can_render = false;
          if (image_data && image_data_size > 0)
          {
            int dummy_w = 0;
            int dummy_h = 0;
            can_render = renderer->get_image_size(state.epub_list[i].cover_path, image_data, image_data_size, &dummy_w, &dummy_h);
          }
          if (can_render)
          {
            renderer->set_image_placeholder_enabled(false);
            renderer->draw_image(state.epub_list[i].cover_path, image_data, image_data_size, image_xpos, image_ypos, image_width, image_height);
            renderer->set_image_placeholder_enabled(true);
          }
          else
          {
            needs_title_card = true;
          }
        }
        else
        {
          needs_title_card = true;
        }

        if (needs_title_card)
        {
          // Draw a bordered title container in place of the cover.
          int card_x = image_xpos;
          int card_y = image_ypos;
          int card_w = image_width;
          int card_h = image_height;
          if (card_w > 0 && card_h > 0)
          {
            renderer->fill_rect(card_x, card_y, card_w, card_h, 255);
            renderer->draw_rect(card_x, card_y, card_w, card_h, 0);
            int inner_padding = 4;
            int inner_x = card_x + inner_padding;
            int inner_y = card_y + inner_padding;
            int inner_w = card_w - inner_padding * 2;
            int inner_h = card_h - inner_padding * 2;
            if (inner_w > 0 && inner_h > 0)
            {
              // Use bold for the title in the list-view title card.
              renderer->draw_text_box(state.epub_list[i].title, inner_x, inner_y, inner_w, inner_h, true, false);
            }
          }
        }

        // image_buffer frees memory
        // draw the title
        int text_xpos = image_xpos + image_width + PADDING;
        int text_ypos = ypos + PADDING / 2;
        int text_width = page_width - (text_xpos + PADDING);
        int text_height = cell_height - PADDING * 2;
        // use the text block to layout the title
        TextBlock *title_block = m_title_blocks[i];
        if (!title_block)
        {
          title_block = new TextBlock(LEFT_ALIGN);
          // Render library titles in bold in the list view.
          title_block->add_span(state.epub_list[i].title, true, false);
          title_block->layout(renderer, &epub, text_width);
          m_title_blocks[i] = title_block;
        }
        // work out the height of the title
        int title_height = title_block->line_breaks.size() * renderer->get_line_height();
        // center the title in the cell
        int y_offset = title_height < text_height ? (text_height - title_height) / 2 : 0;
        // draw each line of the title making sure we don't run over the cell
        for (int li = 0; li < title_block->line_breaks.size() && y_offset + renderer->get_line_height() < text_height; li++)
        {
          title_block->render(renderer, li, text_xpos, text_ypos + y_offset);
          y_offset += renderer->get_line_height();
        }
        // Yield between list items to keep the watchdog happy when
        // rendering many large covers and titles.
        vTaskDelay(1);
      }
      // clear the selection box around the previous selected item
      if (state.previous_selected_item == i)
      {
        for (int line = 0; line < 5; line++)
        {
          renderer->draw_rect(line, ypos + PADDING / 2 + line, page_width - 2 * line, cell_height - PADDING - 2 * line, 255);
        }
      }
      // draw the selection box around the current selection
      if (state.selected_item == i)
      {
        for (int line = 0; line < 5; line++)
        {
          renderer->draw_rect(line, ypos + PADDING / 2 + line, page_width - 2 * line, cell_height - PADDING - 2 * line, 0);
        }
      }
      ypos += cell_height;
    }
  }

  // draw bottom navigation / toggle bar
  if (bottom_bar_height > 0 && bottom_bar_height <= page_height)
  {
    int bar_y = page_height - bottom_bar_height;
    renderer->fill_rect(0, bar_y, page_width, bottom_bar_height, 255);
    int center_x = page_width / 2;
    int center_y = bar_y + bottom_bar_height / 2;

    int total_pages = 1;
    if (state.num_epubs > 0 && items_per_page > 0)
    {
      total_pages = (state.num_epubs + items_per_page - 1) / items_per_page;
    }
    if (total_pages < 1)
    {
      total_pages = 1;
    }
    int page_display = current_page + 1;
    if (page_display < 1)
    {
      page_display = 1;
    }
    if (page_display > total_pages)
    {
      page_display = total_pages;
    }

    // Layout "<<      <      Page X of Y      >      >>" so that the
    // arrow groups are bold with ~50% more spacing between single and
    // double arrows, while the "Page X of Y" text remains normal.
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

    int space_w = renderer->get_space_width();
    if (space_w < 0)
    {
      space_w = 0;
    }
    // Use 6 spaces (~50% more than the previous 4-space gaps).
    int gap_arrows = space_w * 6;
    int gap_center = space_w * 6;

    // Divide the full page width into five equal navigation regions
    // that span the entire bottom bar: [<<] [<] [X / Y] [>] [>>]. Place
    // the text baseline slightly below geometric center to account for
    // how epdiy bitmap glyphs sit relative to the baseline.
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
    int rd_zone_end = page_width; // consume any remainder to reach the edge

    int bar_top = bar_y;
    int bar_bottom = bar_y + bottom_bar_height;
    int bar_height = bar_bottom - bar_top;
    if (bar_height < renderer->get_line_height() + 4)
    {
      bar_height = renderer->get_line_height() + 4;
    }

    auto draw_nav_box = [&](int x0, int x1) {
      if (x1 <= x0)
      {
        return;
      }
      int box_x = x0;
      int box_w = x1 - x0;
      int box_y = bar_top + 2;
      int box_h = bar_height - 4;
      if (box_h <= 0)
      {
        box_h = bar_height;
      }
      renderer->draw_rect(box_x, box_y, box_w, box_h, 0);
    };

    // Outline each interactive region so the user can see the
    // navigation buttons that correspond to the touch zones.
    draw_nav_box(ld_zone_start, ld_zone_end);
    draw_nav_box(ls_zone_start, ls_zone_end);
    draw_nav_box(center_zone_start, center_zone_end);
    draw_nav_box(rs_zone_start, rs_zone_end);
    draw_nav_box(rd_zone_start, rd_zone_end);

    // Center each label within its corresponding region.
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

    // Draw each segment: bold arrows, normal "Page X of Y".
    renderer->draw_text(x_ld, label_y, left_double, true, false);
    renderer->draw_text(x_ls, label_y, left_single, true, false);
    renderer->draw_text(x_center, label_y, center, false, false);
    renderer->draw_text(x_rs, label_y, right_single, true, false);
    renderer->draw_text(x_rd, label_y, right_double, true, false);
  }

  state.previous_selected_item = state.selected_item;
  state.previous_rendered_page = current_page;
}

// Binary index format:
// uint32_t magic ('EBIX'), uint16_t version (1), uint16_t count,
// followed by count EpubListItem records.
bool EpubList::load_index(const char *books_path, const char *index_path)
{
  FILE *fp = fopen(index_path, "rb");
  if (!fp)
  {
    return false;
  }
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t count = 0;
  if (fread(&magic, sizeof(magic), 1, fp) != 1 ||
      fread(&version, sizeof(version), 1, fp) != 1 ||
      fread(&count, sizeof(count), 1, fp) != 1)
  {
    fclose(fp);
    return false;
  }
  const uint32_t EXPECTED_MAGIC = 0x58494245; // 'EBIX'
  if (magic != EXPECTED_MAGIC || version != 1 || count == 0 || count > MAX_EPUB_LIST_SIZE)
  {
    fclose(fp);
    return false;
  }
  if (fread(state.epub_list, sizeof(EpubListItem), count, fp) != count)
  {
    fclose(fp);
    return false;
  }
  fclose(fp);

  // Quick validation: count EPUB-like files in books_path and ensure it matches
  int dir_count = 0;
  DIR *dir = opendir(books_path);
  if (dir)
  {
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL)
    {
      if (ent->d_name[0] == '.' || ent->d_name[0] == '_' || ent->d_type == DT_DIR)
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
      bool is_epu = (e0 == 'e' && e1 == 'p' && e2 == 'u' && (ext[3] == '\0'));
      bool is_epub = (e0 == 'e' && e1 == 'p' && e2 == 'u' && e3 == 'b' && ext[4] == '\0');
      if (!is_epu && !is_epub)
      {
        continue;
      }
      dir_count++;
    }
    closedir(dir);
  }
  if (dir_count != count)
  {
    return false;
  }

  state.num_epubs = count;
  state.is_loaded = true;
  state.previous_rendered_page = -1;
  state.previous_selected_item = -1;
  if (state.selected_item >= state.num_epubs)
  {
    state.selected_item = 0;
  }
  return true;
}

void EpubList::save_index(const char *index_path)
{
  char path_buf[MAX_PATH_SIZE];
  strncpy(path_buf, index_path, sizeof(path_buf));
  path_buf[sizeof(path_buf) - 1] = '\0';
  char *last_slash = strrchr(path_buf, '/');
  if (last_slash)
  {
    *last_slash = '\0';
    struct stat st;
    if (stat(path_buf, &st) != 0)
    {
      mkdir(path_buf, 0775);
    }
  }
  FILE *fp = fopen(index_path, "wb");
  if (!fp)
  {
    ESP_LOGE(TAG, "Failed to open index file %s for write: errno=%d (%s)", index_path, errno, strerror(errno));
    return;
  }
  const uint32_t magic = 0x58494245; // 'EBIX'
  const uint16_t version = 1;
  uint16_t count = static_cast<uint16_t>(state.num_epubs);
  if (fwrite(&magic, sizeof(magic), 1, fp) != 1 ||
      fwrite(&version, sizeof(version), 1, fp) != 1 ||
      fwrite(&count, sizeof(count), 1, fp) != 1)
  {
    ESP_LOGE(TAG, "Failed to write index header to %s", index_path);
    fclose(fp);
    return;
  }
  if (count > 0)
  {
    if (fwrite(state.epub_list, sizeof(EpubListItem), count, fp) != count)
    {
      ESP_LOGE(TAG, "Failed to write index entries to %s", index_path);
    }
  }
  fclose(fp);
}

std::string EpubList::get_cache_path(const char *epub_path)
{
  std::string s = epub_path;
  for (size_t i = 0; i < s.length(); i++)
  {
    if (s[i] == '/')
    {
      s[i] = '_';
    }
  }
  return "/fs/cache/" + s + ".raw";
}

void EpubList::ensure_cache_dir()
{
  struct stat st;
  if (stat("/fs/cache", &st) != 0)
  {
    mkdir("/fs/cache", 0775);
  }
}