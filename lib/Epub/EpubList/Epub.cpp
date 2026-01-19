#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef UNIT_TEST
#include <esp_log.h>
#else
#define ESP_LOGI(tag, args...) \
  printf(args);                \
  printf("\n");
#define ESP_LOGE(tag, args...) \
  printf(args);                \
  printf("\n");
#define ESP_LOGD(tag, args...) \
  printf(args);                \
  printf("\n");
#define ESP_LOGW(tag, args...) \
  printf(args);                \
  printf("\n");
#endif
#include <map>
#include <pugixml.hpp>
#include "../ZipFile/ZipFile.h"
#include "Epub.h"

static const char *TAG = "EPUB";

bool Epub::find_content_opf_file(ZipFile &zip, std::string &content_opf_file)
{
  // open up the meta data to find where the content.opf file lives
  char *meta_info = (char *)zip.read_file_to_memory("META-INF/container.xml");
  if (!meta_info)
  {
    ESP_LOGE(TAG, "Could not find META-INF/container.xml");
    return false;
  }
  // Parse META-INF/container.xml using PugiXML
  pugi::xml_document doc;
  pugi::xml_parse_result result = doc.load_buffer(meta_info, strlen(meta_info));
  free(meta_info);
  if (!result)
  {
    ESP_LOGE(TAG, "Could not parse META-INF/container.xml: %s", result.description());
    return false;
  }

  pugi::xml_node container = doc.child("container");
  if (!container)
  {
    ESP_LOGE(TAG, "Could not find container element in META-INF/container.xml");
    return false;
  }
  pugi::xml_node rootfiles = container.child("rootfiles");
  if (!rootfiles)
  {
    ESP_LOGE(TAG, "Could not find rootfiles element in META-INF/container.xml");
    return false;
  }
  // find the root file that has the media-type="application/oebps-package+xml"
  for (pugi::xml_node rootfile = rootfiles.child("rootfile"); rootfile; rootfile = rootfile.next_sibling("rootfile"))
  {
    const char *media_type = rootfile.attribute("media-type").value();
    if (media_type && strcmp(media_type, "application/oebps-package+xml") == 0)
    {
      const char *full_path = rootfile.attribute("full-path").value();
      if (full_path && full_path[0] != '\0')
      {
        content_opf_file = full_path;
        return true;
      }
    }
  }
  ESP_LOGE(TAG, "Could not get path to content.opf file");
  return false;
}

bool Epub::parse_content_opf(ZipFile &zip, std::string &content_opf_file)
{
  // read in the content.opf file and parse it
  char *contents = (char *)zip.read_file_to_memory(content_opf_file.c_str());
  if (!contents)
  {
    ESP_LOGE(TAG, "Failed to read content.opf '%s'", content_opf_file.c_str());
    return false;
  }

  pugi::xml_document doc;
  pugi::xml_parse_result result = doc.load_buffer(contents, strlen(contents));
  free(contents);
  if (!result)
  {
    ESP_LOGE(TAG, "Error parsing content.opf (pugixml): %s", result.description());
    return false;
  }

  pugi::xml_node package = doc.child("package");
  if (!package)
  {
    ESP_LOGE(TAG, "Could not find package element in content.opf");
    return false;
  }

  // get the metadata - title and cover image
  pugi::xml_node metadata = package.child("metadata");
  if (!metadata)
  {
    ESP_LOGE(TAG, "Missing metadata");
    return false;
  }
  pugi::xml_node title = metadata.child("dc:title");
  if (!title || !title.child_value())
  {
    ESP_LOGE(TAG, "Missing title");
    return false;
  }
  m_title = title.child_value();

  // find <meta name="cover" content="..."> if present
  pugi::xml_node cover = metadata.find_child_by_attribute("meta", "name", "cover");
  const char *cover_item = cover ? cover.attribute("content").value() : nullptr;
  if (!cover)
  {
    ESP_LOGW(TAG, "Missing cover");
  }

  // read the manifest and spine
  pugi::xml_node manifest = package.child("manifest");
  if (!manifest)
  {
    ESP_LOGE(TAG, "Missing manifest");
    return false;
  }

  std::map<std::string, std::string> items;
  for (pugi::xml_node item = manifest.child("item"); item; item = item.next_sibling("item"))
  {
    const char *id_attr = item.attribute("id").value();
    const char *href_attr = item.attribute("href").value();
    if (!id_attr || !href_attr)
    {
      continue;
    }
    std::string item_id = id_attr;
    std::string href = m_base_path + href_attr;

    if (cover_item && item_id == cover_item)
    {
      m_cover_image_item = href;
    }
    if (item_id == "ncx")
    {
      m_toc_ncx_item = href;
    }
    items[item_id] = href;
  }

  pugi::xml_node spine = package.child("spine");
  if (!spine)
  {
    ESP_LOGE(TAG, "Missing spine");
    return false;
  }
  for (pugi::xml_node itemref = spine.child("itemref"); itemref; itemref = itemref.next_sibling("itemref"))
  {
    const char *idref = itemref.attribute("idref").value();
    if (!idref)
    {
      continue;
    }
    auto it = items.find(idref);
    if (it != items.end())
    {
      m_spine.push_back(std::make_pair(std::string(idref), it->second));
    }
  }
  return true;
}

bool Epub::parse_toc_ncx_file(ZipFile &zip)
{
  // the ncx file should have been specified in the content.opf file
  if (m_toc_ncx_item.empty())
  {
    ESP_LOGE(TAG, "No ncx file specified");
    return false;
  }
  ESP_LOGI(TAG, "toc path: %s\n", m_toc_ncx_item.c_str());

  char *ncx_data = (char *)zip.read_file_to_memory(m_toc_ncx_item.c_str());
  if (!ncx_data)
  {
    ESP_LOGE(TAG, "Could not find %s", m_toc_ncx_item.c_str());
    return false;
  }

  pugi::xml_document doc;
  pugi::xml_parse_result result = doc.load_buffer(ncx_data, strlen(ncx_data));
  free(ncx_data);
  if (!result)
  {
    ESP_LOGE(TAG, "Error parsing toc (pugixml): %s", result.description());
    return false;
  }

  pugi::xml_node ncx = doc.child("ncx");
  if (!ncx)
  {
    ESP_LOGE(TAG, "Could not find first child ncx in toc");
    return false;
  }

  pugi::xml_node navMap = ncx.child("navMap");
  if (!navMap)
  {
    ESP_LOGE(TAG, "Could not find navMap child in ncx");
    return false;
  }

  for (pugi::xml_node navPoint = navMap.child("navPoint"); navPoint; navPoint = navPoint.next_sibling("navPoint"))
  {
    pugi::xml_node navLabel = navPoint.child("navLabel").child("text");
    const char *title_text = navLabel.child_value();
    if (!title_text)
    {
      continue;
    }
    std::string title = title_text;

    pugi::xml_node content = navPoint.child("content");
    const char *src = content.attribute("src").value();
    if (!src)
    {
      continue;
    }
    std::string href = m_base_path + src;

    size_t pos = href.find('#');
    std::string anchor;
    if (pos != std::string::npos)
    {
      anchor = href.substr(pos + 1);
      href = href.substr(0, pos);
    }
    m_toc.push_back(EpubTocEntry(title, href, anchor, 0));
  }
  return true;
}

Epub::Epub(const std::string &path) : m_path(path)
{
}

// load in the meta data for the epub file
bool Epub::load()
{
  ZipFile zip(m_path.c_str());
  std::string content_opf_file;
  if (!find_content_opf_file(zip, content_opf_file))
  {
    ESP_LOGE(TAG, "Could not open ePub '%s'", m_path.c_str());
    return false;
  }
  // get the base path for the content
  m_base_path = content_opf_file.substr(0, content_opf_file.find_last_of('/') + 1);
  if (!parse_content_opf(zip, content_opf_file))
  {
    return false;
  }
  // The NCX table of contents is optional for our purposes: many modern
  // EPUB3 files use different navigation structures. If parsing the NCX
  // fails (e.g. "No ncx file specified"), log the error inside
  // parse_toc_ncx_file() but still allow the EPUB to be treated as
  // successfully loaded so it can be listed and read.
  if (!parse_toc_ncx_file(zip))
  {
    ESP_LOGW(TAG, "Continuing without NCX table of contents for '%s'", m_path.c_str());
  }
  return true;
}

const std::string &Epub::get_title()
{
  return m_title;
}

const std::string &Epub::get_cover_image_item()
{
  return m_cover_image_item;
}

std::string normalise_path(const std::string &path)
{
  std::vector<std::string> components;
  std::string component;
  for (auto c : path)
  {
    if (c == '/')
    {
      if (!component.empty())
      {
        if (component == "..")
        {
          if (!components.empty())
          {
            components.pop_back();
          }
        }
        else
        {
          components.push_back(component);
        }
        component.clear();
      }
    }
    else
    {
      component += c;
    }
  }
  if (!component.empty())
  {
    components.push_back(component);
  }
  std::string result;
  for (auto &component : components)
  {
    if (result.size() > 0)
    {
      result += "/";
    }
    result += component;
  }
  return result;
}

uint8_t *Epub::get_item_contents(const std::string &item_href, size_t *size)
{
  ZipFile zip(m_path.c_str());
  std::string path = normalise_path(item_href);
  auto content = zip.read_file_to_memory(path.c_str(), size);
  if (!content)
  {
    ESP_LOGE(TAG, "Failed to read item %s", path.c_str());
    return nullptr;
  }
  return content;
}

int Epub::get_spine_items_count()
{
  return m_spine.size();
}

std::string &Epub::get_spine_item(int spine_index)
{
  static std::string empty_spine_item;
  if (m_spine.empty())
  {
    ESP_LOGI(TAG, "get_spine_item called with empty spine");
    return empty_spine_item;
  }

  if (spine_index < 0 || spine_index >= static_cast<int>(m_spine.size()))
  {
    ESP_LOGI(TAG, "get_spine_item index:%d is out_of_range", spine_index);
    spine_index = 0;
  }
  // if exception is catched return last page
  return m_spine[spine_index].second;
}

EpubTocEntry &Epub::get_toc_item(int toc_index)
{
  return m_toc[toc_index];
}

int Epub::get_toc_items_count()
{
  return m_toc.size();
}

// work out the section index for a toc index
int Epub::get_spine_index_for_toc_index(int toc_index)
{
  if (m_toc.empty() || m_spine.empty())
  {
    ESP_LOGI(TAG, "get_spine_index_for_toc_index called with empty toc or spine");
    return 0;
  }
  if (toc_index < 0)
  {
    toc_index = 0;
  }
  else if (toc_index >= static_cast<int>(m_toc.size()))
  {
    ESP_LOGI(TAG, "toc_index %d out of range (max %d), clamping", toc_index, static_cast<int>(m_toc.size()) - 1);
    toc_index = static_cast<int>(m_toc.size()) - 1;
  }
  // the toc entry should have an href that matches the spine item
  // so we can find the spine index by looking for the href
  for (int i = 0; i < static_cast<int>(m_spine.size()); i++)
  {
    if (m_spine[i].second == m_toc[toc_index].href)
    {
      return i;
    }
  }
  ESP_LOGI(TAG, "Section not found");
  // not found - default to the start of the book
  return 0;
}

std::vector<uint8_t> Epub::get_item_contents_as_vector(const std::string &item_href)
{
  size_t size = 0;
  uint8_t *data = get_item_contents(item_href, &size);
  if (!data)
  {
    return {};
  }
  std::vector<uint8_t> vec(data, data + size);
  free(data);
  return vec;
}
