#include "plugin_api.h"

#include <Windows.h>

#ifndef S2K_VERSION
#define S2K_VERSION "dev"
#endif

namespace {
const S2kHostApi* g_host{};

bool initialize(const S2kHostApi* host) {
    if (!host || host->abi_version != S2K_PLUGIN_ABI_VERSION) return false;
    g_host = host;
    return true;
}

void update() {
    // GUI-independent tweak automation runs here on every presented frame.
}

void render() {
    if (!g_host || !g_host->begin_main_menu_bar()) return;

    if (g_host->begin_menu("Settings", true)) {
        g_host->text("Interface hotkey");
        g_host->separator();
        const char* hotkey_label = g_host->hotkey_capture_active()
            ? "Press a key combination... (Esc cancels)"
            : g_host->hotkey_text();
        if (g_host->button(hotkey_label, 330.0f, 38.0f)) g_host->begin_hotkey_capture();
        g_host->text_disabled("Click, then press e.g. F9 or Alt+0");
        g_host->text_disabled("Choose a combination that does not conflict with game controls.");

        g_host->spacing();
        g_host->separator();
        g_host->text("Interface font size");
        float scale = g_host->font_scale();
        g_host->set_next_item_width(330.0f);
        if (g_host->slider_float("##InterfaceFontScale", &scale, 0.75f, 2.50f, "%.2fx"))
            g_host->set_font_scale(scale);
        g_host->same_line();
        if (g_host->button("Reset##FontScale", 0.0f, 0.0f)) g_host->set_font_scale(1.0f);

        g_host->spacing();
        g_host->separator();
        g_host->text("Notification popup");
        float notification_x = g_host->notification_x_percent();
        float notification_y = g_host->notification_y_percent();
        float notification_duration = g_host->notification_duration_seconds();
        g_host->set_next_item_width(330.0f);
        if (g_host->slider_float("X position##Notification", &notification_x, 0.0f, 100.0f, "%.0f%%"))
            g_host->set_notification_position(notification_x, notification_y);
        g_host->set_next_item_width(330.0f);
        if (g_host->slider_float("Y position##Notification", &notification_y, 0.0f, 100.0f, "%.0f%%"))
            g_host->set_notification_position(notification_x, notification_y);
        g_host->set_next_item_width(330.0f);
        if (g_host->slider_float("Duration##Notification", &notification_duration, 0.5f, 30.0f, "%.1f s"))
            g_host->set_notification_duration(notification_duration);
        if (g_host->button("Preview notification", 180.0f, 30.0f))
            g_host->show_notification("s2k_Tweaks notification preview", S2kNotificationKind::info);
        g_host->text_disabled("Position is clamped so the popup always stays on-screen.");

        g_host->spacing();
        g_host->separator();
        g_host->text("Live update");
        g_host->text_disabled("Loaded module: s2k_Tweaks v" S2K_VERSION);
        if (g_host->button("Reload addon", 180.0f, 32.0f)) g_host->request_reload();
        g_host->text_disabled("Replace s2k_Tweaks.dll, then reload. The game can stay open.");
        g_host->end_menu();
    }

    if (g_host->begin_menu("Tweaks", true)) {
        if (g_host->begin_menu("Video", true)) {
            g_host->text_disabled("No video tweaks configured yet.");
            g_host->end_menu();
        }
        if (g_host->begin_menu("Physics", true)) {
            g_host->text_disabled("No physics tweaks configured yet.");
            g_host->end_menu();
        }
        g_host->end_menu();
    }

    if (g_host->begin_menu("Cheats", true)) {
        bool unlocked = g_host->console_unlocked();
        const bool available = g_host->console_available();
        g_host->begin_disabled(!available);
        if (g_host->checkbox("Unlock CVARs and console commands", &unlocked))
            g_host->set_console_unlocked(unlocked);
        g_host->end_disabled();
        g_host->text_disabled(available
            ? "Running process only; the game EXE is not modified."
            : "Known console signature was not found.");
        g_host->end_menu();
    }

    g_host->end_main_menu_bar();
}

void shutdown() { g_host = nullptr; }

const S2kPluginApi g_api{
    S2K_PLUGIN_ABI_VERSION,
    "s2k_Tweaks",
    S2K_VERSION,
    initialize,
    update,
    render,
    shutdown
};
}

extern "C" __declspec(dllexport) const S2kPluginApi* s2k_get_plugin_api(
    std::uint32_t host_abi_version) {
    return host_abi_version == S2K_PLUGIN_ABI_VERSION ? &g_api : nullptr;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
