#include "EpubToc.h"

static const char *TAG = "PUBINDEX";
#define PADDING 14
#define ITEMS_PER_PAGE 6

EpubToc::~EpubToc()
{
  for (auto *block : m_title_blocks)
  {
    delete block;
  }
  m_title_blocks.clear();
  delete epub;
}

void EpubToc::next()
{
  // must be loaded as we need the information from the epub
  if (!epub)
  {
    load();
  }
  state.selected_item = (state.selected_item + 1) % epub->get_toc_items_count();
}

void EpubToc::prev()
{
  // must be loaded as we need the information from the epub
  if (!epub)
  {
    load();
  }
  state.selected_item = (state.selected_item - 1 + epub->get_toc_items_count()) % epub->get_toc_items_count();
}

bool EpubToc::load()
{
  ESP_LOGI(TAG, "load");

  if (!epub || epub->get_path() != selected_epub.path)
  {
    renderer->show_busy();
    delete epub;

    epub = new Epub(selected_epub.path);
    if (!epub->load())
    {
      ESP_LOGE(TAG, "Failed to load epub for index: %s", selected_epub.path);
      return false;
    }
  }
  // If there is no TOC, signal failure so callers can fall back to
  // opening the book directly without an index.
  if (epub->get_toc_items_count() == 0)
  {
    ESP_LOGW(TAG, "No TOC entries available for %s", selected_epub.path);
    return false;
  }
  state.num_items = epub->get_toc_items_count();
  if (state.num_items <= 0)
  {
    state.num_items = 0;
    state.selected_item = 0;
  }
  else
  {
    if (state.selected_item < 0)
    {
      state.selected_item = 0;
    }
    else if (state.selected_item >= state.num_items)
    {
      state.selected_item = state.num_items - 1;
    }
  }
  if (state.num_items > 0)
  {
    if (m_title_blocks.size() < static_cast<size_t>(state.num_items))
    {
      m_title_blocks.resize(state.num_items, nullptr);
    }
  }
  else
  {
    m_title_blocks.clear();
  }
  ESP_LOGI(TAG, "Epub index loaded");
  return true;
}

// TODO - this is currently pretty much a copy of the epub list rendering
// we can fit a lot more on the screen by allowing variable cell heights
// and a lot of the optimisations that are used for the list aren't really
// required as we're not rendering thumbnails
void EpubToc::render()
{
  ESP_LOGD(TAG, "Rendering EPUB index");
  // For FreeType-backed renderers (e.g. Paper S3), temporarily
  // increase the reading font size while drawing the TOC so entries
  // are easier to tap and read. We restore the original size on exit.
#ifdef USE_FREETYPE
  int original_px = renderer->get_reading_font_pixel_height();
  bool size_changed = false;
  if (original_px > 0)
  {
    int toc_px = original_px * 2;
    size_changed = renderer->set_reading_font_pixel_height(toc_px);
  }
#endif
  int page_width = renderer->get_page_width();
  int page_height = renderer->get_page_height();
  if (page_width <= 0 || page_height <= 0)
  {
    return;
  }

  int bottom_bar_height = EPUB_LIST_BOTTOM_BAR_HEIGHT;
  int content_height = page_height;
  if (bottom_bar_height > 0 && bottom_bar_height < page_height)
  {
    content_height = page_height - bottom_bar_height;
  }
  // what page are we on?
  int current_page = 0;
  if (state.num_items > 0)
  {
    current_page = state.selected_item / ITEMS_PER_PAGE;
  }
  // show five items per page
  int cell_height = content_height / ITEMS_PER_PAGE;
  int start_index = current_page * ITEMS_PER_PAGE;
  int ypos = 0;
  // starting a fresh page or rendering from scratch?
  ESP_LOGI(TAG, "Current page is %d, previous page %d, redraw=%d", current_page, state.previous_rendered_page, m_needs_redraw);
  if (current_page != state.previous_rendered_page || m_needs_redraw)
  {
    m_needs_redraw = false;
    renderer->clear_screen();
    state.previous_selected_item = -1;
    // trigger a redraw of the items
    state.previous_rendered_page = -1;
  }
  for (int i = start_index; i < start_index + ITEMS_PER_PAGE && i < epub->get_toc_items_count(); i++)
  {
    // do we need to draw a new page of items?
    if (current_page != state.previous_rendered_page)
    {
      TextBlock *title_block = nullptr;
      if (i >= 0 && i < static_cast<int>(m_title_blocks.size()))
      {
        title_block = m_title_blocks[i];
      }
      if (!title_block)
      {
        title_block = new TextBlock(LEFT_ALIGN);
        title_block->add_span(epub->get_toc_item(i).title.c_str(), false, false);
        title_block->layout(renderer, epub, renderer->get_page_width());
        if (i >= 0 && i < static_cast<int>(m_title_blocks.size()))
        {
          m_title_blocks[i] = title_block;
        }
      }
      // work out the height of the title
      int text_height = cell_height - PADDING;
      int title_height = title_block->line_breaks.size() * renderer->get_line_height();
      // center the title in the cell
      int y_offset = title_height < text_height ? (text_height - title_height) / 2 : 0;
      // draw each line of the index block making sure we don't run over the cell
      int height = 0;
      for (int i = 0; i < title_block->line_breaks.size() && height < text_height; i++)
      {
        title_block->render(renderer, i, 10, ypos + height + y_offset);
        height += renderer->get_line_height();
      }
    }
    // clear the selection box around the previous selected item
    if (state.previous_selected_item == i)
    {
      for (int line = 0; line < 3; line++)
      {
        renderer->draw_rect(line, ypos + PADDING / 2 + line, page_width - 2 * line, cell_height - PADDING - 2 * line, 255);
      }
    }
    // draw the selection box around the current selection
    if (state.selected_item == i)
    {
      for (int line = 0; line < 3; line++)
      {
        renderer->draw_rect(line, ypos + PADDING / 2 + line, page_width - 2 * line, cell_height - PADDING - 2 * line, 0);
      }
    }
    ypos += cell_height;
  }
  state.previous_selected_item = state.selected_item;
  state.previous_rendered_page = current_page;

  // draw bottom navigation bar
  if (bottom_bar_height > 0 && bottom_bar_height <= page_height)
  {
    int bar_y = page_height - bottom_bar_height;
    renderer->fill_rect(0, bar_y, page_width, bottom_bar_height, 255);
    int center_x = page_width / 2;
    int center_y = bar_y + bottom_bar_height / 2;

    int total_pages = 1;
    if (state.num_items > 0)
    {
      total_pages = (state.num_items + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
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
    // the text baseline slightly below geometric center so the glyphs
    // appear visually centered in the bar.
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

#ifdef USE_FREETYPE
  // Restore the original reading font size after TOC rendering so the
  // main reading view keeps its configured size.
  if (size_changed)
  {
    renderer->set_reading_font_pixel_height(original_px);
  }
#endif
}

uint16_t EpubToc::get_selected_toc()
{
  return epub->get_spine_index_for_toc_index(state.selected_item);
}