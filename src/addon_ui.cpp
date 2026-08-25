#include "addon_ui.h"

#include "console_unlock.h"
#include "log.h"
#include "notification_state.h"
#include "plugin_manager.h"

#include <imgui.h>
#include <backends/imgui_impl_win32.h>

#include <atomic>
#include <array>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <format>
#include <mutex>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
struct Hotkey {
    UINT vk{VK_F10};
    bool ctrl{};
    bool alt{};
    bool shift{};
};

HWND g_window{};
WNDPROC g_original_wndproc{};
std::atomic_bool g_menu_open{};
std::atomic_bool g_capture_hotkey{};
Hotkey g_hotkey{};
std::mutex g_hotkey_mutex;
bool g_hotkey_was_down{};
std::array<bool, 256> g_capture_key_was_down{};
std::once_flag g_load_config_once;
float g_font_scale{1.0f};
float g_notification_x_percent{50.0f};
float g_notification_y_percent{12.0f};
float g_notification_duration_seconds{3.5f};
bool g_console_unlocked_preference{true};

Hotkey current_hotkey() {
    std::scoped_lock lock(g_hotkey_mutex);
    return g_hotkey;
}

void replace_hotkey(const Hotkey& hotkey) {
    std::scoped_lock lock(g_hotkey_mutex);
    g_hotkey = hotkey;
}

std::filesystem::path config_path() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return std::filesystem::path(path).parent_path() / L"s2k_Tweaks.ini";
}

void save_config() {
    const auto hotkey = current_hotkey();
    std::ofstream file(config_path(), std::ios::trunc);
    if (!file) return;
    file << "hotkey_vk=" << hotkey.vk << '\n';
    file << "hotkey_ctrl=" << hotkey.ctrl << '\n';
    file << "hotkey_alt=" << hotkey.alt << '\n';
    file << "hotkey_shift=" << hotkey.shift << '\n';
    file << "font_scale=" << g_font_scale << '\n';
    file << "notification_x_percent=" << g_notification_x_percent << '\n';
    file << "notification_y_percent=" << g_notification_y_percent << '\n';
    file << "notification_duration_seconds=" << g_notification_duration_seconds << '\n';
    file << "console_unlocked=" << g_console_unlocked_preference << '\n';
}

void load_config() {
    auto hotkey = current_hotkey();
    std::ifstream file(config_path());
    std::string line;
    while (std::getline(file, line)) {
        const auto split = line.find('=');
        if (split == std::string::npos) continue;
        const auto key = line.substr(0, split);
        const int value = std::atoi(line.c_str() + split + 1);
        if (key == "hotkey_vk" && value > 0 && value < 256) hotkey.vk = static_cast<UINT>(value);
        else if (key == "hotkey_ctrl") hotkey.ctrl = value != 0;
        else if (key == "hotkey_alt") hotkey.alt = value != 0;
        else if (key == "hotkey_shift") hotkey.shift = value != 0;
        else if (key == "font_scale") {
            try {
                g_font_scale = std::stof(line.substr(split + 1));
            } catch (...) {
                dda::log("Invalid font scale in config; using the default");
            }
        }
        else if (key == "notification_x_percent") {
            try { g_notification_x_percent = std::stof(line.substr(split + 1)); }
            catch (...) { dda::log("Invalid notification X position in config"); }
        }
        else if (key == "notification_y_percent") {
            try { g_notification_y_percent = std::stof(line.substr(split + 1)); }
            catch (...) { dda::log("Invalid notification Y position in config"); }
        }
        else if (key == "notification_duration_seconds") {
            try { g_notification_duration_seconds = std::stof(line.substr(split + 1)); }
            catch (...) { dda::log("Invalid notification duration in config"); }
        }
        else if (key == "console_unlocked") g_console_unlocked_preference = value != 0;
    }
    g_font_scale = std::clamp(g_font_scale, 0.75f, 2.50f);
    g_notification_x_percent = std::clamp(g_notification_x_percent, 0.0f, 100.0f);
    g_notification_y_percent = std::clamp(g_notification_y_percent, 0.0f, 100.0f);
    g_notification_duration_seconds = std::clamp(g_notification_duration_seconds, 0.5f, 30.0f);
    replace_hotkey(hotkey);
}

bool async_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

std::string key_name(UINT vk) {
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (vk == VK_LEFT || vk == VK_UP || vk == VK_RIGHT || vk == VK_DOWN || vk == VK_PRIOR ||
        vk == VK_NEXT || vk == VK_END || vk == VK_HOME || vk == VK_INSERT || vk == VK_DELETE ||
        vk == VK_DIVIDE || vk == VK_NUMLOCK) scan |= 0x100;
    char name[64]{};
    if (GetKeyNameTextA(static_cast<LONG>(scan << 16), name, sizeof(name))) return name;
    return std::format("VK {}", vk);
}

std::string hotkey_text() {
    const auto hotkey = current_hotkey();
    std::string result;
    if (hotkey.ctrl) result += "CTRL + ";
    if (hotkey.alt) result += "ALT + ";
    if (hotkey.shift) result += "SHIFT + ";
    return result + key_name(hotkey.vk);
}

bool is_modifier(UINT vk) {
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_MENU ||
           vk == VK_LMENU || vk == VK_RMENU || vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT;
}

void commit_hotkey(UINT vk) {
    replace_hotkey({vk, async_down(VK_CONTROL), async_down(VK_MENU), async_down(VK_SHIFT)});
    g_capture_hotkey = false;
    g_hotkey_was_down = true;
    save_config();
    dda::log(std::format("GUI hotkey changed to {}", hotkey_text()));
}

void poll_mouse_input() {
    auto& io = ImGui::GetIO();
    POINT cursor{};
    if (g_window && GetForegroundWindow() == g_window && GetCursorPos(&cursor) && ScreenToClient(g_window, &cursor))
        io.AddMousePosEvent(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
    else
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    io.AddMouseButtonEvent(0, async_down(VK_LBUTTON));
    io.AddMouseButtonEvent(1, async_down(VK_RBUTTON));
    io.AddMouseButtonEvent(2, async_down(VK_MBUTTON));
}

void poll_hotkey_capture() {
    for (UINT vk = 1; vk < 256; ++vk) {
        const bool down = async_down(static_cast<int>(vk));
        const bool first_press = down && !g_capture_key_was_down[vk];
        g_capture_key_was_down[vk] = down;
        if (!g_capture_hotkey.load() || !first_press) continue;
        if (vk == VK_ESCAPE) {
            g_capture_hotkey = false;
            continue;
        }
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || is_modifier(vk)) continue;
        commit_hotkey(vk);
    }
}

LRESULT CALLBACK hooked_wndproc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    const bool key_down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool first_press = key_down && (lparam & (1LL << 30)) == 0;
    if (first_press) {
        const auto vk = static_cast<UINT>(wparam);
        if (g_capture_hotkey.load()) {
            if (vk == VK_ESCAPE) {
                g_capture_hotkey = false;
            } else if (!is_modifier(vk)) {
                commit_hotkey(vk);
            }
            return 0;
        }
    }
    if (g_menu_open.load()) {
        ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
        switch (message) {
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR:
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_INPUT:
            return 0;
        default: break;
        }
    }
    return CallWindowProcW(g_original_wndproc, window, message, wparam, lparam);
}
} // namespace

namespace dda {
void load_addon_preferences() {
    std::call_once(g_load_config_once, load_config);
}

bool addon_console_unlock_preference() {
    load_addon_preferences();
    return g_console_unlocked_preference;
}

std::string addon_hotkey_text() { return hotkey_text(); }
bool addon_hotkey_capture_active() { return g_capture_hotkey.load(); }
void begin_addon_hotkey_capture() { g_capture_hotkey = true; }
float addon_font_scale() { return g_font_scale; }
float addon_notification_x_percent() { load_addon_preferences(); return g_notification_x_percent; }
float addon_notification_y_percent() { load_addon_preferences(); return g_notification_y_percent; }
float addon_notification_duration_seconds() { load_addon_preferences(); return g_notification_duration_seconds; }

void set_addon_font_scale(float value) {
    g_font_scale = std::clamp(value, 0.75f, 2.50f);
    if (ImGui::GetCurrentContext()) ImGui::GetIO().FontGlobalScale = g_font_scale;
    save_config();
}

void set_addon_notification_position(float x_percent, float y_percent) {
    g_notification_x_percent = std::clamp(x_percent, 0.0f, 100.0f);
    g_notification_y_percent = std::clamp(y_percent, 0.0f, 100.0f);
    save_config();
}

void set_addon_notification_duration(float seconds) {
    g_notification_duration_seconds = std::clamp(seconds, 0.5f, 30.0f);
    save_config();
}

bool set_addon_console_unlocked(bool enabled) {
    if (!set_console_commands_unlocked(enabled)) {
        log("GUI console checkbox change failed");
        return false;
    }
    g_console_unlocked_preference = enabled;
    save_config();
    return true;
}

bool initialize_addon_ui(HWND window) {
    if (!window) return false;
    g_window = window;
    load_addon_preferences();
    if (!ImGui_ImplWin32_Init(window)) return false;
    ImGui::GetIO().FontGlobalScale = g_font_scale;
    initialize_plugin_manager();
    SetLastError(0);
    g_original_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hooked_wndproc)));
    if (!g_original_wndproc && GetLastError() != 0) {
        ImGui_ImplWin32_Shutdown();
        return false;
    }
    log(std::format("Dear ImGui Win32 input initialized; GUI hotkey={}", hotkey_text()));
    return true;
}

void addon_ui_new_frame() {
    ImGui_ImplWin32_NewFrame();
    poll_mouse_input();
    poll_hotkey_capture();
}
bool addon_ui_wants_render() { return g_menu_open.load(); }
void close_addon_ui() { g_menu_open = false; }

void poll_addon_ui_hotkey() {
    if (g_capture_hotkey.load()) return;
    const auto hotkey = current_hotkey();
    const bool key_down = async_down(static_cast<int>(hotkey.vk));
    const bool modifiers_match = async_down(VK_CONTROL) == hotkey.ctrl &&
        async_down(VK_MENU) == hotkey.alt && async_down(VK_SHIFT) == hotkey.shift;
    const bool combination_down = key_down && modifiers_match;
    if (combination_down && !g_hotkey_was_down) {
        const bool opening = !g_menu_open.load();
        g_menu_open = opening;
    }
    g_hotkey_was_down = combination_down;
}

void render_addon_ui() {
    if (!g_menu_open.load()) return;
    auto& io = ImGui::GetIO();
    io.MouseDrawCursor = true;
    render_plugin_ui();
}

void shutdown_addon_ui() {
    g_menu_open = false;
    notification_state().dismiss();
    if (ImGui::GetCurrentContext()) ImGui::GetIO().MouseDrawCursor = false;
    if (g_window && g_original_wndproc)
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_wndproc));
    ImGui_ImplWin32_Shutdown();
    g_window = nullptr;
    g_original_wndproc = nullptr;
    g_capture_hotkey = false;
    log("GUI closed for renderer transition");
}
} // namespace dda
