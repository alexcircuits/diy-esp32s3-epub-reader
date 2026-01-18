#pragma once

#include <stdint.h>

const int MAX_EPUB_LIST_SIZE = 20;
const int MAX_PATH_SIZE = 256;
const int MAX_TITLE_SIZE = 100;

const int EPUB_GRID_ROWS = 3;
const int EPUB_GRID_COLUMNS = 3;
const int EPUB_LIST_ITEMS_PER_PAGE = 5;
const int EPUB_TOC_ITEMS_PER_PAGE = 6;
const int EPUB_LIST_BOTTOM_BAR_HEIGHT = 80;

// nice and simple state that can be persisted easily
typedef struct
{
  char path[MAX_PATH_SIZE];
  char title[MAX_TITLE_SIZE];
  uint16_t current_section;
  uint16_t current_page;
  uint16_t pages_in_current_section;
  char cover_path[MAX_PATH_SIZE];
} EpubListItem;

// this is held in the RTC memory
typedef struct
{
  int previous_rendered_page;
  int previous_selected_item;
  int selected_item;
  int num_epubs;
  bool is_loaded;
  bool use_grid_view;
  EpubListItem epub_list[MAX_EPUB_LIST_SIZE];
} EpubListState;

// this is held in the RTC memory
typedef struct
{
  int previous_rendered_page;
  int previous_selected_item;
  int selected_item;
  int num_items;
} EpubTocState;
