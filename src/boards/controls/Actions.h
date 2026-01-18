#pragma once

#include <functional>

typedef enum
{
  NONE,
  UP,
  DOWN,
  SELECT,
  LAST_INTERACTION,
  PREV_SECTION,
  NEXT_SECTION,
  TOGGLE_STATUS_BAR,
  REFRESH_PAGE,
  OPEN_READER_MENU
} UIAction;

typedef std::function<void(UIAction)> ActionCallback_t;
