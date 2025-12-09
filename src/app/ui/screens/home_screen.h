/*
 * Home Screen
 * 
 * Device status dashboard showing:
 * - Device name / hostname
 * - Network info (WiFi signal, IP address)
 * - System stats (CPU usage, temperature, memory)
 * - Firmware version
 * 
 * Updates every 1 second via LVGL timer.
 */

#ifndef HOME_SCREEN_H
#define HOME_SCREEN_H

#include "../base_screen.h"

class HomeScreen : public BaseScreen {
 public:
  lv_obj_t* root() override;
  void onEnter() override;
  void onExit() override;
  void handle(const UiEvent &evt) override;
  void cleanup() override;

 private:
  void build();
  void updateStats();
  static void timerCallback(lv_timer_t* timer);
  
  lv_obj_t* root_ = nullptr;
  lv_timer_t* update_timer_ = nullptr;
  
  // UI elements
  lv_obj_t* device_name_label_ = nullptr;
  lv_obj_t* wifi_signal_label_ = nullptr;
  lv_obj_t* ip_address_label_ = nullptr;
  lv_obj_t* cpu_usage_label_ = nullptr;
  lv_obj_t* temperature_label_ = nullptr;
  lv_obj_t* memory_label_ = nullptr;
  lv_obj_t* uptime_label_ = nullptr;
  lv_obj_t* version_label_ = nullptr;
};

#endif // HOME_SCREEN_H
