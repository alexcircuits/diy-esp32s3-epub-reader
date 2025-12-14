#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <stdint.h>
#include "controls/ButtonControls.h"
#include "controls/TouchControls.h"
#include "battery/Battery.h"

class SDCard;
class Renderer;

class Board
{
public:
  // Filesystem backing store for the board (Paper S3 uses SD card only)
  SDCard *sdcard;

  // override this to do any startup tasks required for your board
  // e.g. turning on the epd, enabling power to peripherals, etc...
  virtual void power_up() = 0;
  // override this to do any shutdown tasks required for your board
  // e.g. turning off the epd, disabling power to peripherals, etc...
  virtual void prepare_to_sleep() = 0;
  // get the renderer for your board
  virtual Renderer *get_renderer() = 0;
  // start up the filesystem - for Paper S3 this always mounts an SD card at /fs
  virtual void start_filesystem();
  // stop the filesystem
  virtual void stop_filesystem();
  // get the battery monitoring object - the default behaviour is to use the built-in
  // ADC if BATTERY_ADC_CHANNEL is defined
  virtual Battery *get_battery();

  // get the button controls object
  virtual ButtonControls *get_button_controls(QueueHandle_t ui_queue) = 0;

  // implement this to return a TouchControls object for your board - the default behaviour returns
  // a dummy implementation that does nothing
  virtual TouchControls *get_touch_controls(Renderer *renderer, QueueHandle_t ui_queue);

  // factory method to create a new instance of the board - now always returns a PaperS3
  static Board *factory();
};