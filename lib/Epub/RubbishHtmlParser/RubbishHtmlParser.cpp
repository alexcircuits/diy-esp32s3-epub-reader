#ifndef UNIT_TEST
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#else
#define vTaskDelay(t)
#define ESP_LOGE(args...)
#define ESP_LOGI(args...)
#endif
#include <stdio.h>
#include <string.h>
#include <string>
#include <list>
#include <vector>
#include <functional>
#include <exception>
#include <ctype.h>
#include "../ZipFile/ZipFile.h"
#include "../Renderer/Renderer.h"
#include "htmlEntities.h"
#include "blocks/TextBlock.h"
#include "blocks/ImageBlock.h"
#include "Page.h"
#include "RubbishHtmlParser.h"
#include "../EpubList/Epub.h"

static const char *TAG = "HTML";

const char *HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
const int NUM_HEADER_TAGS = sizeof(HEADER_TAGS) / sizeof(HEADER_TAGS[0]);

const char *BLOCK_TAGS[] = {"p", "li", "div", "br"};
const int NUM_BLOCK_TAGS = sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]);

const char *BOLD_TAGS[] = {"b", "strong"};
const int NUM_BOLD_TAGS = sizeof(BOLD_TAGS) / sizeof(BOLD_TAGS[0]);

const char *ITALIC_TAGS[] = {"i", "em"};
const int NUM_ITALIC_TAGS = sizeof(ITALIC_TAGS) / sizeof(ITALIC_TAGS[0]);

const char *IMAGE_TAGS[] = {"img"};
const int NUM_IMAGE_TAGS = sizeof(IMAGE_TAGS) / sizeof(IMAGE_TAGS[0]);

const char *SKIP_TAGS[] = {"head", "table"};
const int NUM_SKIP_TAGS = sizeof(SKIP_TAGS) / sizeof(SKIP_TAGS[0]);

// given the start and end of a tag, check to see if it matches a known tag
bool matches(const char *tag_name, const char *possible_tags[], int possible_tag_count)
{
  for (int i = 0; i < possible_tag_count; i++)
  {
    if (strcmp(tag_name, possible_tags[i]) == 0)
    {
      return true;
    }
  }
  return false;
}

BLOCK_STYLE parse_text_align_from_style(const char *style_attr, BLOCK_STYLE default_style, bool treat_justify_as_justified)
{
  if (!style_attr)
  {
    return default_style;
  }
  std::string s(style_attr);
  for (size_t i = 0; i < s.size(); i++)
  {
    s[i] = tolower(static_cast<unsigned char>(s[i]));
  }
  size_t pos = s.find("text-align");
  if (pos == std::string::npos)
  {
    return default_style;
  }
  size_t colon = s.find(':', pos);
  if (colon == std::string::npos)
  {
    return default_style;
  }
  size_t start = colon + 1;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t'))
  {
    start++;
  }
  size_t end = start;
  while (end < s.size() && s[end] != ';')
  {
    end++;
  }
  std::string value = s.substr(start, end - start);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
  {
    value.pop_back();
  }
  if (value == "left")
  {
    return LEFT_ALIGN;
  }
  if (value == "center")
  {
    return CENTER_ALIGN;
  }
  if (value == "right")
  {
    return RIGHT_ALIGN;
  }
  if (value == "justify")
  {
    // Map fully-justified paragraphs to left-align by default to
    // avoid overly large gaps between words, unless the caller has
    // explicitly requested justification.
    return treat_justify_as_justified ? JUSTIFIED : LEFT_ALIGN;
  }
  return default_style;
}

RubbishHtmlParser::RubbishHtmlParser(const char *html, int length, const std::string &base_path, bool justify_paragraphs)
    : m_justify_paragraphs(justify_paragraphs)
{
  m_base_path = base_path;
  parse(html, length);
}

RubbishHtmlParser::~RubbishHtmlParser()
{
  for (auto block : blocks)
  {
    delete block;
  }
}

bool RubbishHtmlParser::enter_node(const pugi::xml_node &element)
{
  const char *tag_name = element.name();
  // we only handle image tags
  if (matches(tag_name, IMAGE_TAGS, NUM_IMAGE_TAGS))
  {
    const char *src = element.attribute("src").value();
    if (src)
    {
      // don't leave an empty text block in the list
      BLOCK_STYLE style = currentTextBlock->get_style();
      if (currentTextBlock->is_empty())
      {
        blocks.pop_back();
        delete currentTextBlock;
        currentTextBlock = nullptr;
      }
      blocks.push_back(new ImageBlock(m_base_path + src));
      // start a new text block - with the same style as before
      startNewTextBlock(style);
    }
    else
    {
      ESP_LOGE(TAG, "Could not find src attribute");
    }
  }
  else if (matches(tag_name, SKIP_TAGS, NUM_SKIP_TAGS))
  {
    return false;
  }
  else if (matches(tag_name, HEADER_TAGS, NUM_HEADER_TAGS))
  {
    is_bold = true;
    startNewTextBlock(CENTER_ALIGN);
  }
  else if (matches(tag_name, BLOCK_TAGS, NUM_BLOCK_TAGS))
  {
    if (strcmp(tag_name, "br") == 0)
    {
      BLOCK_STYLE style = JUSTIFIED;
      if (currentTextBlock)
      {
        style = currentTextBlock->get_style();
      }
      startNewTextBlock(style);
    }
    else
    {
      // Default to either left-aligned or fully-justified paragraphs
      // depending on the reader setting, but still honour explicit
      // CSS text-align where present.
      BLOCK_STYLE default_style = m_justify_paragraphs ? JUSTIFIED : LEFT_ALIGN;
      const char *style_attr = element.attribute("style").value();
      BLOCK_STYLE style = parse_text_align_from_style(style_attr, default_style, m_justify_paragraphs);
      startNewTextBlock(style);
    }
  }
  else if (matches(tag_name, BOLD_TAGS, NUM_BOLD_TAGS))
  {
    is_bold = true;
  }
  else if (matches(tag_name, ITALIC_TAGS, NUM_ITALIC_TAGS))
  {
    is_italic = true;
  }
  return true;
}
/// Visit a text node.
bool RubbishHtmlParser::visit_text(const pugi::xml_node &node)
{
  const char *value = node.value();
  if (value && value[0] != '\0')
  {
    addText(value, is_bold, is_italic);
  }
  return true;
}
bool RubbishHtmlParser::exit_node(const pugi::xml_node &element)
{
  const char *tag_name = element.name();
  if (matches(tag_name, HEADER_TAGS, NUM_HEADER_TAGS))
  {
    is_bold = false;
  }
  else if (matches(tag_name, BLOCK_TAGS, NUM_BLOCK_TAGS))
  {
    // nothing to do
  }
  else if (matches(tag_name, BOLD_TAGS, NUM_BOLD_TAGS))
  {
    is_bold = false;
  }
  else if (matches(tag_name, ITALIC_TAGS, NUM_ITALIC_TAGS))
  {
    is_italic = false;
  }
  return true;
}

// start a new text block if needed
void RubbishHtmlParser::startNewTextBlock(BLOCK_STYLE style)
{
  if (currentTextBlock)
  {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->is_empty())
    {
      currentTextBlock->set_style(style);
      return;
    }
    else
    {
      currentTextBlock->finish();
    }
  }
  currentTextBlock = new TextBlock(style);
  blocks.push_back(currentTextBlock);
}

void RubbishHtmlParser::parse(const char *html, int length)
{
  // Default paragraph alignment is controlled by the reader setting.
  startNewTextBlock(m_justify_paragraphs ? JUSTIFIED : LEFT_ALIGN);
  pugi::xml_document doc;
  // Preserve significant text while still collapsing some whitespace via
  // our own addText() handling.
  pugi::xml_parse_result result =
      doc.load_buffer(html, length, pugi::parse_default | pugi::parse_ws_pcdata);

  if (!result)
  {
    // If parsing fails, just treat everything as a single text block.
    addText(html, false, false);
    return;
  }

  pugi::xml_node root = doc.child("html");
  pugi::xml_node body = root ? root.child("body") : pugi::xml_node();
  pugi::xml_node start = body ? body : doc;

  std::function<void(const pugi::xml_node &)> walk =
      [&](const pugi::xml_node &node)
      {
        if (node.type() == pugi::node_element)
        {
          if (!enter_node(node))
          {
            return;
          }
          for (pugi::xml_node child : node.children())
          {
            walk(child);
          }
          exit_node(node);
        }
        else if (node.type() == pugi::node_pcdata || node.type() == pugi::node_cdata)
        {
          visit_text(node);
        }
      };

  for (pugi::xml_node child : start.children())
  {
    walk(child);
  }
}

void RubbishHtmlParser::addText(const char *text, bool is_bold, bool is_italic)
{
  // Probably there is a more elegant way to do this
  std::string parsetxt = replace_html_entities((string)text);
  currentTextBlock->add_span(parsetxt.c_str(), is_bold, is_italic);
}

static const int MAX_IMAGES_PER_SECTION = 8;

void RubbishHtmlParser::layout(Renderer *renderer, Epub *epub)
{
  const int line_height = renderer->get_line_height();
  const int page_height = renderer->get_page_height();
  // first ask the blocks to work out where they should have
  // line breaks based on the page width
  int images_seen = 0;
  for (auto block : blocks)
  {
    if (block->getType() == BlockType::IMAGE_BLOCK)
    {
      images_seen++;
      if (images_seen > MAX_IMAGES_PER_SECTION)
      {
        ImageBlock *imageBlock = (ImageBlock *)block;
        imageBlock->width = 0;
        imageBlock->height = 0;
        // feed the watchdog even when skipping heavy images
        vTaskDelay(1);
        continue;
      }
    }

    block->layout(renderer, epub);
    // feed the watchdog
    vTaskDelay(1);
  }
  // now we need to allocate the lines to pages
  // we'll run through each block and the lines within each block and allocate
  // them to pages. When we run out of space on a page we'll start a new page
  // and continue
  int y = 0;
  pages.push_back(new Page());
  for (auto block : blocks)
  {
    // feed the watchdog
    vTaskDelay(1);
    if (block->getType() == BlockType::TEXT_BLOCK)
    {
      TextBlock *textBlock = (TextBlock *)block;
      for (int line_break_index = 0; line_break_index < textBlock->line_breaks.size(); line_break_index++)
      {
        if (y + line_height > page_height)
        {
          pages.push_back(new Page());
          y = 0;
        }
        pages.back()->elements.push_back(new PageLine(textBlock, line_break_index, y));
        y += line_height;
      }
      // add some extra line between blocks
      y += line_height * 0.5;
    }
    if (block->getType() == BlockType::IMAGE_BLOCK)
    {
      ImageBlock *imageBlock = (ImageBlock *)block;
      if (imageBlock->width <= 0 || imageBlock->height <= 0)
      {
        continue;
      }
      if (y + imageBlock->height > page_height)
      {
        pages.push_back(new Page());
        y = 0;
      }
      pages.back()->elements.push_back(new PageImage(imageBlock, y));
      y += imageBlock->height;
    }
  }
}

void RubbishHtmlParser::render_page(int page_index, Renderer *renderer, Epub *epub)
{
  renderer->clear_screen();
  // This is presumably needed only for epdiy based devices. @chris let's not do it for others like M5
  if (renderer->has_gray()) {
    // Get up black to clean last fonts before printing a gray page
    //renderer->fill_rect(0, 0, renderer->get_page_width(), renderer->get_page_height(), 0);
    renderer->flush_display();
  }

  if (page_index < 0 || page_index >= static_cast<int>(pages.size()))
  {
    ESP_LOGI(TAG, "render_page out of range");
    // This could be nicer. Notice that last word "button" is cut          v
    uint16_t y = renderer->get_page_height()/2-20;
    renderer->draw_rect(1, y, renderer->get_page_width(), 105, 125);
    renderer->draw_text_box("Reached the limit of the book\nUse the SELECT button",
                            10, y, renderer->get_page_width(), 80, false, false);
    return;
  }

  pages[page_index]->render(renderer, epub);
}
