#include "plugin_manager.h"

#include "addon_ui.h"
#include "console_unlock.h"
#include "log.h"
#include "notification_state.h"
#include "plugin_api.h"

#include <Windows.h>
#include <imgui.h>

#include <atomic>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>

namespace {
struct LoadedPlugin {
    HMODULE module{};
    const S2kPluginApi* api{};
    std::filesystem::path runtime_path;
};

LoadedPlugin g_plugin;
std::mutex g_plugin_mutex;
std::atomic_bool g_reload_requested{};
std::uint64_t g_runtime_generation{};
std::string g_hotkey_label;

std::filesystem::path game_directory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path runtime_directory(std::error_code& error) {
    const auto temporary = std::filesystem::temp_directory_path(error);
    if (error) return {};
    return temporary / "s2k_Tweaks" / std::to_wstring(GetCurrentProcessId());
}

bool host_begin_main_menu_bar() { return ImGui::BeginMainMenuBar(); }
void host_end_main_menu_bar() { ImGui::EndMainMenuBar(); }
bool host_begin_menu(const char* label, bool enabled) { return ImGui::BeginMenu(label, enabled); }
void host_end_menu() { ImGui::EndMenu(); }
void host_text(const char* value) { ImGui::TextUnformatted(value); }
void host_text_disabled(const char* value) { ImGui::TextDisabled("%s", value); }
void host_separator() { ImGui::Separator(); }
void host_spacing() { ImGui::Spacing(); }
void host_same_line() { ImGui::SameLine(); }
void host_set_next_item_width(float width) { ImGui::SetNextItemWidth(width); }
bool host_button(const char* label, float width, float height) {
    return ImGui::Button(label, {width, height});
}
bool host_slider_float(const char* label, float* value, float minimum, float maximum, const char* format) {
    return ImGui::SliderFloat(label, value, minimum, maximum, format);
}
bool host_checkbox(const char* label, bool* value) { return ImGui::Checkbox(label, value); }
void host_begin_disabled(bool disabled) { ImGui::BeginDisabled(disabled); }
void host_end_disabled() { ImGui::EndDisabled(); }
const char* host_hotkey_text() {
    g_hotkey_label = dda::addon_hotkey_text();
    return g_hotkey_label.c_str();
}
bool host_hotkey_capture_active() { return dda::addon_hotkey_capture_active(); }
void host_begin_hotkey_capture() { dda::begin_addon_hotkey_capture(); }
float host_font_scale() { return dda::addon_font_scale(); }
void host_set_font_scale(float value) { dda::set_addon_font_scale(value); }
float host_notification_x_percent() { return dda::addon_notification_x_percent(); }
float host_notification_y_percent() { return dda::addon_notification_y_percent(); }
float host_notification_duration_seconds() { return dda::addon_notification_duration_seconds(); }
void host_set_notification_position(float x, float y) { dda::set_addon_notification_position(x, y); }
void host_set_notification_duration(float seconds) { dda::set_addon_notification_duration(seconds); }
void host_show_notification(const char* message, S2kNotificationKind kind) {
    if (!message) return;
    dda::NotificationKind core_kind = dda::NotificationKind::info;
    switch (kind) {
    case S2kNotificationKind::success: core_kind = dda::NotificationKind::success; break;
    case S2kNotificationKind::warning: core_kind = dda::NotificationKind::warning; break;
    case S2kNotificationKind::error: core_kind = dda::NotificationKind::error; break;
    default: break;
    }
    dda::notification_state().show(
        message, core_kind, dda::addon_notification_duration_seconds());
}
bool host_console_available() { return dda::console_unlock_available(); }
bool host_console_unlocked() { return dda::console_commands_unlocked(); }
bool host_set_console_unlocked(bool enabled) { return dda::set_addon_console_unlocked(enabled); }
void host_request_reload() { dda::request_plugin_reload(); }

const S2kHostApi g_host_api{
    S2K_PLUGIN_ABI_VERSION,
    host_begin_main_menu_bar,
    host_end_main_menu_bar,
    host_begin_menu,
    host_end_menu,
    host_text,
    host_text_disabled,
    host_separator,
    host_spacing,
    host_same_line,
    host_set_next_item_width,
    host_button,
    host_slider_float,
    host_checkbox,
    host_begin_disabled,
    host_end_disabled,
    host_hotkey_text,
    host_hotkey_capture_active,
    host_begin_hotkey_capture,
    host_font_scale,
    host_set_font_scale,
    host_notification_x_percent,
    host_notification_y_percent,
    host_notification_duration_seconds,
    host_set_notification_position,
    host_set_notification_duration,
    host_show_notification,
    host_console_available,
    host_console_unlocked,
    host_set_console_unlocked,
    host_request_reload
};

bool load_new_plugin() {
    const auto source = game_directory() / L"s2k_Tweaks.dll";
    std::error_code error;
    if (!std::filesystem::is_regular_file(source, error)) {
        dda::log(std::format("Reloadable addon module was not found: {}", source.string()));
        return false;
    }

    const auto runtime_dir = runtime_directory(error);
    if (error || runtime_dir.empty()) {
        dda::log(std::format("Could not resolve addon runtime directory ({})", error.message()));
        return false;
    }
    std::filesystem::create_directories(runtime_dir, error);
    if (error) {
        dda::log(std::format("Could not create addon runtime directory ({})", error.message()));
        return false;
    }
    const auto runtime = runtime_dir /
        std::format(L"s2k_Tweaks_runtime_{}.dll", ++g_runtime_generation);
    if (!std::filesystem::copy_file(source, runtime, std::filesystem::copy_options::overwrite_existing, error)) {
        dda::log(std::format("Could not shadow-copy addon module ({})", error.message()));
        return false;
    }

    HMODULE module = LoadLibraryW(runtime.c_str());
    if (!module) {
        dda::log(std::format("Loading addon module failed ({})", GetLastError()));
        std::filesystem::remove(runtime, error);
        return false;
    }
    const auto get_api = reinterpret_cast<S2kGetPluginApi>(
        GetProcAddress(module, "s2k_get_plugin_api"));
    const S2kPluginApi* api = get_api ? get_api(S2K_PLUGIN_ABI_VERSION) : nullptr;
    if (!api || api->abi_version != S2K_PLUGIN_ABI_VERSION || !api->initialize ||
        !api->update || !api->render || !api->shutdown || !api->initialize(&g_host_api)) {
        dda::log("Addon module rejected because its API is missing, incompatible, or failed initialization");
        FreeLibrary(module);
        std::filesystem::remove(runtime, error);
        return false;
    }

    LoadedPlugin previous = g_plugin;
    g_plugin = {module, api, runtime};
    dda::log(std::format("Reloadable addon loaded: {} version {}", api->name, api->version));
    dda::notification_state().show(
        std::format("{} v{} - PLUGIN LOADED", api->name, api->version),
        dda::NotificationKind::success, dda::addon_notification_duration_seconds());
    if (previous.api) previous.api->shutdown();
    if (previous.module) FreeLibrary(previous.module);
    if (!previous.runtime_path.empty()) std::filesystem::remove(previous.runtime_path, error);
    return true;
}

void render_missing_plugin_menu() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("Settings")) {
        ImGui::TextDisabled("s2k_Tweaks.dll is missing or incompatible.");
        if (ImGui::Button("Reload addon", {180.0f, 32.0f})) g_reload_requested = true;
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}
}

namespace dda {
bool initialize_plugin_manager() {
    std::scoped_lock lock(g_plugin_mutex);
    if (g_plugin.api) return true;
    return load_new_plugin();
}

void request_plugin_reload() { g_reload_requested = true; }

void update_plugin() {
    std::scoped_lock lock(g_plugin_mutex);
    if (g_reload_requested.exchange(false)) load_new_plugin();
    if (g_plugin.api) g_plugin.api->update();
}

void render_plugin_ui() {
    std::scoped_lock lock(g_plugin_mutex);
    if (g_plugin.api) g_plugin.api->render();
    else render_missing_plugin_menu();
}

}
