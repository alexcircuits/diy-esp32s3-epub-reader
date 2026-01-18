#pragma once

#include "TouchControls.h"
#include "Actions.h"

#include <stdint.h>
#include <functional>

class Renderer;

// Touch controls implementation for the Paper S3 using the GT911
// capacitive touch controller. This uses the "old" ESP-IDF I2C
// driver APIs (i2c_driver_install / i2c_master_*) to avoid
// conflicts with epdiy's use of the legacy I2C driver.
class PaperS3TouchControls : public TouchControls
{
public:
  PaperS3TouchControls(Renderer *renderer, ActionCallback_t on_action);

  // Update gesture sensitivity profile (0=low,1=medium,2=high).
  static void set_gesture_profile(int profile_index);

  // draw any visual touch hints (currently a no-op)
  virtual void render(Renderer *renderer) override;

  // show pressed state feedback (currently a no-op)
  virtual void renderPressedState(Renderer *renderer, UIAction action, bool state = true) override;

private:
  static void touchTask(void *param);
  void loop();
  bool readTouchPoint(uint16_t *x, uint16_t *y, uint8_t *points);
  UIAction mapTapToAction(uint16_t x, uint16_t y);
  UIAction mapSwipeUpToAction(uint16_t start_x, uint16_t start_y, uint16_t end_x, uint16_t end_y, uint8_t max_points);
  UIAction mapSwipeDownToAction(uint16_t start_x, uint16_t start_y, uint16_t end_x, uint16_t end_y, uint8_t max_points);
  UIAction mapLongPressToAction(uint16_t x, uint16_t y);

  ActionCallback_t on_action;
  Renderer *renderer;

  bool touch_active = false;
  bool driver_ok = false;
  UIAction last_action = NONE;
  uint8_t i2c_addr = 0x14; // default GT911 address, will probe 0x14/0x5D
  uint32_t touch_start_tick = 0;
  bool long_press_handled = false;
};
