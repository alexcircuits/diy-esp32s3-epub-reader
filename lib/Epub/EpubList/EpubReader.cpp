#include <string.h>
#ifndef UNIT_TEST
#include <esp_log.h>
#include <esp_system.h>
#else
#define ESP_LOGI(args...)
#define ESP_LOGE(args...)
#define ESP_LOGI(args...)
#define ESP_LOGD(args...)
#endif
#include "EpubReader.h"
#include "Epub.h"
#include "../RubbishHtmlParser/RubbishHtmlParser.h"
#include "../Renderer/Renderer.h"

static const char *TAG = "EREADER";

EpubReader::~EpubReader()
{
  delete parser;
  delete next_parser;
  delete epub;
}

bool EpubReader::load()
{
  ESP_LOGD(TAG, "Before epub load: %d", esp_get_free_heap_size());
  // do we need to load the epub?
  if (!epub || epub->get_path() != state.path)
  {
    renderer->show_busy();
    delete epub;
    delete parser;
    delete next_parser;
    parser = nullptr;
    next_parser = nullptr;
    parser_section = -1;
    next_parser_section = -1;
    // make sure we have a valid path before trying to load
    if (state.path[0] == '\0')
    {
      ESP_LOGE(TAG, "EpubReader::load called with empty path");
      return false;
    }

    epub = new Epub(state.path);
    // Epub::load() returns true on success, false on failure
    if (!epub->load())
    {
      ESP_LOGE(TAG, "Failed to load epub '%s'", state.path);
      delete epub;
      epub = nullptr;
      return false;
    }
    ESP_LOGD(TAG, "After epub load: %d", esp_get_free_heap_size());
  }
  return true;
}

void EpubReader::parse_and_layout_current_section()
{
  if (!epub)
  {
    ESP_LOGE(TAG, "parse_and_layout_current_section called with null epub");
    return;
  }

  if (parser && parser_section == state.current_section)
  {
    return;
  }

  if (next_parser && next_parser_section == state.current_section)
  {
    delete parser;
    parser = next_parser;
    parser_section = next_parser_section;
    next_parser = nullptr;
    next_parser_section = -1;
    state.pages_in_current_section = parser->get_page_count();
    // Prefetching the next section can be very expensive on
    // image-heavy books. Skip it to keep TOC -> reader
    // transitions responsive.
    return;
  }

  renderer->show_busy();
  ESP_LOGD(TAG, "Parse and render section %d", state.current_section);
  ESP_LOGD(TAG, "Before read html: %d", esp_get_free_heap_size());

  std::string item = epub->get_spine_item(state.current_section);
  if (item.empty())
  {
    ESP_LOGE(TAG, "No spine item for section %d", state.current_section);
    return;
  }
  std::string base_path = item.substr(0, item.find_last_of('/') + 1);
  char *html = reinterpret_cast<char *>(epub->get_item_contents(item));
  if (!html)
  {
    ESP_LOGE(TAG, "Failed to read HTML for spine item '%s'", item.c_str());
    return;
  }
  ESP_LOGD(TAG, "After read html: %d", esp_get_free_heap_size());
  delete parser;
  parser = new RubbishHtmlParser(html, strlen(html), base_path, use_justified);
  parser_section = state.current_section;
  free(html);
  ESP_LOGD(TAG, "After parse: %d", esp_get_free_heap_size());
  parser->layout(renderer, epub);
  ESP_LOGD(TAG, "After layout: %d", esp_get_free_heap_size());
  state.pages_in_current_section = parser->get_page_count();
}

void EpubReader::prefetch_next_section()
{
  if (!epub)
  {
    return;
  }

  int total_sections = epub->get_spine_items_count();
  int next_section = state.current_section + 1;
  if (next_section >= total_sections)
  {
    return;
  }

  if (next_parser && next_parser_section == next_section)
  {
    return;
  }

  delete next_parser;
  next_parser = nullptr;
  next_parser_section = -1;

  std::string item = epub->get_spine_item(next_section);
  if (item.empty())
  {
    return;
  }
  std::string base_path = item.substr(0, item.find_last_of('/') + 1);
  char *html = reinterpret_cast<char *>(epub->get_item_contents(item));
  if (!html)
  {
    return;
  }

  RubbishHtmlParser *p = new RubbishHtmlParser(html, strlen(html), base_path, use_justified);
  free(html);
  p->layout(renderer, epub);
  next_parser = p;
  next_parser_section = next_section;
}

void EpubReader::next()
{
  state.current_page++;
  if (state.current_page >= state.pages_in_current_section)
  {
    state.current_section++;
    state.current_page = 0;
    delete parser;
    parser = nullptr;
  }
}

void EpubReader::prev()
{
  if (state.current_page == 0)
  {
    if (state.current_section > 0)
    {
      delete parser;
      parser = nullptr;
      state.current_section--;
      ESP_LOGD(TAG, "Going to previous section %d", state.current_section);
      parse_and_layout_current_section();
      state.current_page = state.pages_in_current_section - 1;
      return;
    }
  }
  state.current_page--;
}

void EpubReader::next_section()
{
  if (!epub)
  {
    if (!load())
    {
      return;
    }
  }

  int total_sections = epub->get_spine_items_count();
  if (state.current_section + 1 >= total_sections)
  {
    return;
  }

  state.current_section++;
  state.current_page = 0;
  delete parser;
  parser = nullptr;
  parse_and_layout_current_section();
}

void EpubReader::prev_section()
{
  if (!epub)
  {
    if (!load())
    {
      return;
    }
  }

  if (state.current_section == 0)
  {
    state.current_page = 0;
    return;
  }

  state.current_section--;
  state.current_page = 0;
  delete parser;
  parser = nullptr;
  parse_and_layout_current_section();
}

void EpubReader::render()
{
  if (!parser)
  {
    parse_and_layout_current_section();
  }
  if (!parser)
  {
    ESP_LOGE(TAG, "EpubReader::render called with null parser after layout; aborting render");
    return;
  }
  ESP_LOGD(TAG, "rendering page %d of %d", state.current_page, parser->get_page_count());
  parser->render_page(state.current_page, renderer, epub);

  ESP_LOGD(TAG, "rendered page %d of %d", state.current_page, parser->get_page_count());
  ESP_LOGD(TAG, "after render: %d", esp_get_free_heap_size());
}

void EpubReader::set_state_section(uint16_t current_section) {
  ESP_LOGI(TAG, "go to section:%d", current_section);
  state.current_section = current_section;
  state.current_page = 0;
}

void EpubReader::set_justified(bool justified)
{
  if (use_justified != justified)
  {
    use_justified = justified;
    // Force a re-layout of the current and prefetched sections so
    // that the new alignment takes effect on the next render.
    delete parser;
    parser = nullptr;
    parser_section = -1;
    delete next_parser;
    next_parser = nullptr;
    next_parser_section = -1;
  }
}