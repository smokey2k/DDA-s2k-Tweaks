#pragma once

#include <Windows.h>
#include <string>

namespace dda {
bool initialize_addon_ui(HWND window);
void shutdown_addon_ui();
void load_addon_preferences();
bool addon_console_unlock_preference();
std::string addon_hotkey_text();
bool addon_hotkey_capture_active();
void begin_addon_hotkey_capture();
float addon_font_scale();
void set_addon_font_scale(float value);
float addon_notification_x_percent();
float addon_notification_y_percent();
float addon_notification_duration_seconds();
void set_addon_notification_position(float x_percent, float y_percent);
void set_addon_notification_duration(float seconds);
bool set_addon_console_unlocked(bool enabled);
void poll_addon_ui_hotkey();
void addon_ui_new_frame();
void render_addon_ui();
bool addon_ui_wants_render();
void close_addon_ui();
}
