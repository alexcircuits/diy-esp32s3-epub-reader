#pragma once

#include "Board.h"

class PaperS3 : public Board
{
public:
  virtual void power_up();
  virtual void prepare_to_sleep();
  virtual Renderer *get_renderer();
  virtual ButtonControls *get_button_controls(QueueHandle_t ui_queue);
  virtual TouchControls *get_touch_controls(Renderer *renderer, QueueHandle_t ui_queue);
};
