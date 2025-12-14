#pragma once

#include <string>
#include <list>
#include <vector>
#include <pugixml.hpp>
#include "blocks/TextBlock.h"

using namespace std;

class Page;
class Renderer;
class Epub;

// a very stupid xhtml parser - it will probably work for very simple cases
// but will probably fail for complex ones
class RubbishHtmlParser
{
private:
  bool is_bold = false;
  bool is_italic = false;

  std::list<Block *> blocks;
  TextBlock *currentTextBlock = nullptr;
  std::vector<Page *> pages;

  std::string m_base_path;

  // Whether new paragraph blocks should default to fully-justified
  // layout or remain left-aligned. This is driven by a user-facing
  // reader setting.
  bool m_justify_paragraphs = false;

  // start a new text block if needed
  void startNewTextBlock(BLOCK_STYLE style);

  // PugiXML-based traversal helpers
  bool enter_node(const pugi::xml_node &node);
  bool visit_text(const pugi::xml_node &node);
  bool exit_node(const pugi::xml_node &node);

public:
  RubbishHtmlParser(const char *html, int length, const std::string &base_path, bool justify_paragraphs);
  ~RubbishHtmlParser();

  void parse(const char *html, int length);
  void addText(const char *text, bool is_bold, bool is_italic);
  void layout(Renderer *renderer, Epub *epub);

  int get_page_count()
  {
    return pages.size();
  }
  const std::list<Block *> &get_blocks()
  {
    return blocks;
  }
  void render_page(int page_index, Renderer *renderer, Epub *epub);
};
