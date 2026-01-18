#ifndef UNIT_TEST
#include <esp_log.h>
#if defined(BOARD_HAS_PSRAM)
#include <esp_heap_caps.h>
#endif
#else
#define ESP_LOGE(args...)
#define ESP_LOGI(args...)
#endif
#include "ZipFile.h"

#include "miniz_local.h"

#define TAG "ZIP"

// read a file from the zip file allocating the required memory for the data
uint8_t *ZipFile::read_file_to_memory(const char *filename, size_t *size)
{
  // open up the epub file using miniz
  local_mz_zip_archive zip_archive;
  memset(&zip_archive, 0, sizeof(zip_archive));
  bool status = local_mz_zip_reader_init_file(&zip_archive, m_filename.c_str(), 0);
  if (!status)
  {
    ESP_LOGE(TAG, "local_mz_zip_reader_init_file() failed!\n");
    ESP_LOGE(TAG, "Error %s\n", local_mz_zip_get_error_string(zip_archive.m_last_error));
    return nullptr;
  }
  // find the file
  local_mz_uint32 file_index = 0;
  if (!local_mz_zip_reader_locate_file_v2(&zip_archive, filename, nullptr, 0, &file_index))
  {
    ESP_LOGE(TAG, "Could not find file %s", filename);
    local_mz_zip_reader_end(&zip_archive);
    return nullptr;
  }
  // get the file size - we do this all manually so we can add a null terminator to any strings
  local_mz_zip_archive_file_stat file_stat;
  if (!local_mz_zip_reader_file_stat(&zip_archive, file_index, &file_stat))
  {
    ESP_LOGE(TAG, "local_mz_zip_reader_file_stat() failed!\n");
    ESP_LOGE(TAG, "Error %s\n", local_mz_zip_get_error_string(zip_archive.m_last_error));
    local_mz_zip_reader_end(&zip_archive);
    return nullptr;
  }
  // allocate memory for the file (optionally in PSRAM)
  size_t file_size = file_stat.m_uncomp_size;
  uint8_t *file_data;
#if !defined(UNIT_TEST) && defined(BOARD_HAS_PSRAM)
  file_data = (uint8_t *)heap_caps_calloc(file_size + 1, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  file_data = (uint8_t *)calloc(file_size + 1, 1);
#endif
  if (!file_data)
  {
    ESP_LOGE(TAG, "Failed to allocate memory for %s\n", file_stat.m_filename);
    local_mz_zip_reader_end(&zip_archive);
    return nullptr;
  }
  // read the file
  status = local_mz_zip_reader_extract_to_mem(&zip_archive, file_index, file_data, file_size, 0);
  if (!status)
  {
    ESP_LOGE(TAG, "local_mz_zip_reader_extract_to_mem() failed!\n");
    ESP_LOGE(TAG, "Error %s\n", local_mz_zip_get_error_string(zip_archive.m_last_error));
    free(file_data);
    local_mz_zip_reader_end(&zip_archive);
    return nullptr;
  }
  // Close the archive, freeing any resources it was using
  local_mz_zip_reader_end(&zip_archive);
  // return the size if required
  if (size)
  {
    *size = file_size;
  }
  return file_data;
}
bool ZipFile::read_file_to_file(const char *filename, const char *dest)
{
  local_mz_zip_archive zip_archive;
  memset(&zip_archive, 0, sizeof(zip_archive));
  bool status = local_mz_zip_reader_init_file(&zip_archive, m_filename.c_str(), 0);
  if (!status)
  {
    ESP_LOGE(TAG, "local_mz_zip_reader_init_file() failed!\n");
    ESP_LOGE(TAG, "Error %s\n", local_mz_zip_get_error_string(zip_archive.m_last_error));
    return false;
  }
  // Run through the archive and find the requiested file
  for (int i = 0; i < (int)local_mz_zip_reader_get_num_files(&zip_archive); i++)
  {
    local_mz_zip_archive_file_stat file_stat;
    if (!local_mz_zip_reader_file_stat(&zip_archive, i, &file_stat))
    {
      ESP_LOGE(TAG, "local_mz_zip_reader_file_stat() failed!\n");
      ESP_LOGE(TAG, "Error %s\n", local_mz_zip_get_error_string(zip_archive.m_last_error));
      local_mz_zip_reader_end(&zip_archive);
      return false;
    }
    // is this the file we're looking for?
    if (strcmp(filename, file_stat.m_filename) == 0)
    {
      ESP_LOGI(TAG, "Extracting %s\n", file_stat.m_filename);
      local_mz_zip_reader_extract_file_to_file(&zip_archive, file_stat.m_filename, dest, 0);
      local_mz_zip_reader_end(&zip_archive);
      return true;
    }
  }
  local_mz_zip_reader_end(&zip_archive);
  return false;
}
