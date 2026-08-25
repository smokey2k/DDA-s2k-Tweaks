#pragma once

#include <cstdint>

inline constexpr std::uint32_t S2K_PLUGIN_ABI_VERSION = 3;

enum class S2kNotificationKind : std::uint32_t { info, success, warning, error };

struct S2kHostApi {
    std::uint32_t abi_version;
    bool (*begin_main_menu_bar)();
    void (*end_main_menu_bar)();
    bool (*begin_menu)(const char* label, bool enabled);
    void (*end_menu)();
    void (*text)(const char* value);
    void (*text_disabled)(const char* value);
    void (*separator)();
    void (*spacing)();
    void (*same_line)();
    void (*set_next_item_width)(float width);
    bool (*button)(const char* label, float width, float height);
    bool (*slider_float)(const char* label, float* value, float minimum, float maximum, const char* format);
    bool (*checkbox)(const char* label, bool* value);
    void (*begin_disabled)(bool disabled);
    void (*end_disabled)();

    const char* (*hotkey_text)();
    bool (*hotkey_capture_active)();
    void (*begin_hotkey_capture)();
    float (*font_scale)();
    void (*set_font_scale)(float value);
    float (*notification_x_percent)();
    float (*notification_y_percent)();
    float (*notification_duration_seconds)();
    void (*set_notification_position)(float x_percent, float y_percent);
    void (*set_notification_duration)(float seconds);
    void (*show_notification)(const char* message, S2kNotificationKind kind);
    bool (*console_available)();
    bool (*console_unlocked)();
    bool (*set_console_unlocked)(bool enabled);
    void (*request_reload)();
};

struct S2kPluginApi {
    std::uint32_t abi_version;
    const char* name;
    const char* version;
    bool (*initialize)(const S2kHostApi* host);
    void (*update)();
    void (*render)();
    void (*shutdown)();
};

using S2kGetPluginApi = const S2kPluginApi* (*)(std::uint32_t host_abi_version);
