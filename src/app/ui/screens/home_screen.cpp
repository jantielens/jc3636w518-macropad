/*
 * Home Screen Implementation
 * 
 * Device status dashboard with 1-second refresh timer.
 */

#include "home_screen.h"
#include "../../device_stats.h"
#include "../../web_assets.h"   // PROJECT_DISPLAY_NAME
#include <lvgl.h>

lv_obj_t* HomeScreen::root() {
  if (!root_) build();
  return root_;
}

void HomeScreen::onEnter() {
  // Start 1-second update timer when screen becomes visible
  if (!update_timer_) {
    update_timer_ = lv_timer_create(timerCallback, 1000, this);  // 1000ms = 1 second
  }
  
  // Immediate update on entry
  updateStats();
}

void HomeScreen::onExit() {
  // Stop timer when leaving screen
  if (update_timer_) {
    lv_timer_del(update_timer_);
    update_timer_ = nullptr;
  }
}

void HomeScreen::cleanup() {
  // Stop timer if still running
  if (update_timer_) {
    lv_timer_del(update_timer_);
    update_timer_ = nullptr;
  }
  
  // Reset pointers - LVGL will delete the objects
  root_ = nullptr;
  device_name_label_ = nullptr;
  wifi_signal_label_ = nullptr;
  ip_address_label_ = nullptr;
  cpu_usage_label_ = nullptr;
  temperature_label_ = nullptr;
  memory_label_ = nullptr;
  uptime_label_ = nullptr;
  version_label_ = nullptr;
}

void HomeScreen::build() {
  root_ = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(root_, 0, 0);
  lv_obj_set_style_pad_all(root_, 20, 0);
  
  // Device name at top
  device_name_label_ = lv_label_create(root_);
  lv_label_set_text(device_name_label_, PROJECT_DISPLAY_NAME);
  lv_obj_set_style_text_color(device_name_label_, lv_color_white(), 0);
  lv_obj_set_style_text_align(device_name_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(device_name_label_, lv_pct(90));
  lv_obj_align(device_name_label_, LV_ALIGN_TOP_MID, 0, 10);
  
  // Network section
  lv_obj_t* network_container = lv_obj_create(root_);
  lv_obj_set_size(network_container, lv_pct(90), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(network_container, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_border_width(network_container, 0, 0);
  lv_obj_set_style_pad_all(network_container, 12, 0);
  lv_obj_align(network_container, LV_ALIGN_TOP_MID, 0, 50);
  
  lv_obj_t* network_title = lv_label_create(network_container);
  lv_label_set_text(network_title, "NETWORK");
  lv_obj_set_style_text_color(network_title, lv_color_hex(0x888888), 0);
  lv_obj_align(network_title, LV_ALIGN_TOP_LEFT, 0, 0);
  
  wifi_signal_label_ = lv_label_create(network_container);
  lv_label_set_text(wifi_signal_label_, "WiFi: --");
  lv_obj_set_style_text_color(wifi_signal_label_, lv_color_white(), 0);
  lv_obj_align(wifi_signal_label_, LV_ALIGN_TOP_LEFT, 0, 25);
  
  ip_address_label_ = lv_label_create(network_container);
  lv_label_set_text(ip_address_label_, "IP: --");
  lv_obj_set_style_text_color(ip_address_label_, lv_color_white(), 0);
  lv_obj_align(ip_address_label_, LV_ALIGN_TOP_LEFT, 0, 50);
  
  // System section
  lv_obj_t* system_container = lv_obj_create(root_);
  lv_obj_set_size(system_container, lv_pct(90), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(system_container, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_border_width(system_container, 0, 0);
  lv_obj_set_style_pad_all(system_container, 12, 0);
  lv_obj_align(system_container, LV_ALIGN_TOP_MID, 0, 160);
  
  lv_obj_t* system_title = lv_label_create(system_container);
  lv_label_set_text(system_title, "SYSTEM");
  lv_obj_set_style_text_color(system_title, lv_color_hex(0x888888), 0);
  lv_obj_align(system_title, LV_ALIGN_TOP_LEFT, 0, 0);
  
  cpu_usage_label_ = lv_label_create(system_container);
  lv_label_set_text(cpu_usage_label_, "CPU: --");
  lv_obj_set_style_text_color(cpu_usage_label_, lv_color_white(), 0);
  lv_obj_align(cpu_usage_label_, LV_ALIGN_TOP_LEFT, 0, 25);
  
  temperature_label_ = lv_label_create(system_container);
  lv_label_set_text(temperature_label_, "Temp: --");
  lv_obj_set_style_text_color(temperature_label_, lv_color_white(), 0);
  lv_obj_align(temperature_label_, LV_ALIGN_TOP_LEFT, 0, 50);
  
  memory_label_ = lv_label_create(system_container);
  lv_label_set_text(memory_label_, "Memory: --");
  lv_obj_set_style_text_color(memory_label_, lv_color_white(), 0);
  lv_obj_align(memory_label_, LV_ALIGN_TOP_LEFT, 0, 75);
  
  uptime_label_ = lv_label_create(system_container);
  lv_label_set_text(uptime_label_, "Uptime: --");
  lv_obj_set_style_text_color(uptime_label_, lv_color_white(), 0);
  lv_obj_align(uptime_label_, LV_ALIGN_TOP_LEFT, 0, 100);
  
  // Version at bottom
  version_label_ = lv_label_create(root_);
  lv_label_set_text(version_label_, "v" FIRMWARE_VERSION);
  lv_obj_set_style_text_color(version_label_, lv_color_hex(0x666666), 0);
  lv_obj_set_style_text_align(version_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(version_label_, lv_pct(90));
  lv_obj_align(version_label_, LV_ALIGN_BOTTOM_MID, 0, -10);
}

void HomeScreen::updateStats() {
  if (!root_) return;
  
  // Update device name/hostname
  String hostname = device_stats_get_hostname();
  if (device_name_label_ && hostname.length() > 0) {
    lv_label_set_text(device_name_label_, hostname.c_str());
  }
  
  // Update WiFi signal
  String signal_quality = device_stats_get_wifi_signal_quality();
  int rssi = device_stats_get_wifi_rssi();
  if (wifi_signal_label_) {
    if (rssi != 0) {
      String wifi_text = "WiFi: " + signal_quality + " (" + String(rssi) + " dBm)";
      lv_label_set_text(wifi_signal_label_, wifi_text.c_str());
    } else {
      lv_label_set_text(wifi_signal_label_, "WiFi: Disconnected");
    }
  }
  
  // Update IP address
  String ip = device_stats_get_ip_address();
  if (ip_address_label_) {
    if (ip.length() > 0) {
      String ip_text = "IP: " + ip;
      lv_label_set_text(ip_address_label_, ip_text.c_str());
    } else {
      lv_label_set_text(ip_address_label_, "IP: Not connected");
    }
  }
  
  // Update CPU usage
  int cpu = device_stats_get_cpu_usage();
  if (cpu_usage_label_) {
    if (cpu >= 0) {
      String cpu_text = "CPU: " + String(cpu) + "%";
      lv_label_set_text(cpu_usage_label_, cpu_text.c_str());
    } else {
      lv_label_set_text(cpu_usage_label_, "CPU: --");
    }
  }
  
  // Update temperature
  int temp = device_stats_get_temperature();
  if (temperature_label_) {
    if (temp >= 0) {
      String temp_text = "Temp: " + String(temp) + " C";
      lv_label_set_text(temperature_label_, temp_text.c_str());
    } else {
      lv_label_set_text(temperature_label_, "Temp: N/A");
    }
  }
  
  // Update memory
  String heap_free = device_stats_get_heap_free();
  String heap_size = device_stats_get_heap_size();
  if (memory_label_) {
    String mem_text = "Memory: " + heap_free + " / " + heap_size;
    lv_label_set_text(memory_label_, mem_text.c_str());
  }
  
  // Update uptime
  String uptime = device_stats_get_uptime();
  if (uptime_label_) {
    String uptime_text = "Uptime: " + uptime;
    lv_label_set_text(uptime_label_, uptime_text.c_str());
  }
}

void HomeScreen::timerCallback(lv_timer_t* timer) {
  HomeScreen* screen = static_cast<HomeScreen*>(timer->user_data);
  if (screen) {
    screen->updateStats();
  }
}

void HomeScreen::handle(const UiEvent &evt) {
  // No event handling needed for Phase 3 (touch interaction deferred to Phase 5)
}
