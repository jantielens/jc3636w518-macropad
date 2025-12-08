#include "ui_events.h"
#include <string.h>

static QueueHandle_t ui_event_queue = nullptr;

bool ui_events_init(size_t capacity) {
  if (ui_event_queue) return true; // already initialized
  ui_event_queue = xQueueCreate(capacity, sizeof(UiEvent));
  return ui_event_queue != nullptr;
}

bool ui_publish(const UiEvent &evt) {
  if (!ui_event_queue) return false;
  // Non-blocking send; copy struct by value (payload should be small)
  return xQueueSend(ui_event_queue, &evt, 0) == pdTRUE;
}

bool ui_poll(UiEvent *out_evt) {
  if (!ui_event_queue || !out_evt) return false;
  return xQueueReceive(ui_event_queue, out_evt, 0) == pdTRUE;
}
