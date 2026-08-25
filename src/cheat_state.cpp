#include "cheat_state.h"

#include "log.h"

#include <Windows.h>
#include <MinHook.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <algorithm>
#include <string_view>
#include <vector>

namespace {
struct Section {
    std::uint8_t* begin{};
    std::size_t size{};
    bool executable{};
    bool writable{};
};

std::mutex g_mutex;
bool g_initialized{};
volatile LONG* g_god_value{};
void* g_retail_noclip_command{};
void* g_original_noclip_command{};
bool g_noclip_hook_created{};
bool g_noclip_hook_enabled{};
std::atomic_bool g_noclip_feature_enabled{};
std::atomic_bool g_noclip_runtime_logging{};

using CommandCallback = void(__fastcall*)(void*, void*);

void __fastcall adaptive_noclip_command(void* arguments, void* command_context) {
    if (!command_context) {
        reinterpret_cast<CommandCallback>(g_original_noclip_command)(arguments, command_context);
        return;
    }
    auto* runtime_type = reinterpret_cast<volatile LONG*>(
        reinterpret_cast<std::uint8_t*>(command_context) + 0x14e8);
    const LONG current = *runtime_type;
    if (g_noclip_runtime_logging.load(std::memory_order_relaxed)) {
        auto** vtable = *reinterpret_cast<void***>(command_context);
        const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto vtable_rva = vtable ? reinterpret_cast<std::uintptr_t>(vtable) - module : 0;
        const auto type_function_rva = vtable && vtable[9]
            ? reinterpret_cast<std::uintptr_t>(vtable[9]) - module : 0;
        dda::log(std::format(
            "Noclip runtime debug: type={}, vtableRva=0x{:X}, typeFunctionRva=0x{:X}",
            current, vtable_rva, type_function_rva));
    }
    if (!g_noclip_feature_enabled.load(std::memory_order_relaxed)) {
        reinterpret_cast<CommandCallback>(g_original_noclip_command)(arguments, command_context);
        return;
    }
    if (current == 7) {
        reinterpret_cast<CommandCallback>(g_original_noclip_command)(arguments, command_context);
        return;
    }
    if ((current != 1 && current != 2) ||
        InterlockedCompareExchange(runtime_type, 7, current) != current) {
        dda::log(std::format("Retail Noclip refused unsupported runtime type {}", current));
        return;
    }
    dda::log(std::format("Retail Noclip adapted runtime type {} to 7", current));
    reinterpret_cast<CommandCallback>(g_original_noclip_command)(arguments, command_context);
    if (InterlockedCompareExchange(runtime_type, current, 7) != 7)
        dda::log("Retail Noclip runtime type restoration validation failed");
}

std::vector<Section> image_sections() {
    std::vector<Section> result;
    auto* module = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    if (!module) return result;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return result;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return result;
    const auto image_size = static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage);
    const auto* header = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++header) {
        if ((header->Characteristics & IMAGE_SCN_MEM_READ) == 0) continue;
        const auto offset = static_cast<std::size_t>(header->VirtualAddress);
        if (offset >= image_size) continue;
        result.push_back({
            module + offset,
            (std::min)(static_cast<std::size_t>(header->Misc.VirtualSize), image_size - offset),
            (header->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0,
            (header->Characteristics & IMAGE_SCN_MEM_WRITE) != 0});
    }
    return result;
}

std::vector<std::uint8_t*> find_bytes(
    const std::uint8_t* pattern, const std::uint8_t* mask, std::size_t length,
    bool executable, bool writable) {
    std::vector<std::uint8_t*> hits;
    for (const auto& section : image_sections()) {
        if (section.executable != executable || (writable && !section.writable) || section.size < length)
            continue;
        for (std::size_t i = 0; i + length <= section.size; ++i) {
            bool matches = true;
            for (std::size_t j = 0; j < length; ++j) {
                if (mask[j] && section.begin[i + j] != pattern[j]) {
                    matches = false;
                    break;
                }
            }
            if (matches) hits.push_back(section.begin + i);
        }
    }
    return hits;
}

std::vector<std::uint8_t*> find_string(std::string_view value) {
    std::vector<std::uint8_t> pattern(value.begin(), value.end());
    pattern.push_back(0);
    std::vector<std::uint8_t> mask(pattern.size(), 0xff);
    return find_bytes(pattern.data(), mask.data(), pattern.size(), false, false);
}

template <typename T>
T read_at(const std::uint8_t* address) {
    T value{};
    std::memcpy(&value, address, sizeof(value));
    return value;
}

bool exchange_long(volatile LONG* target, LONG expected, LONG desired) {
    DWORD old_protection{};
    if (!VirtualProtect(const_cast<LONG*>(target), sizeof(LONG), PAGE_READWRITE, &old_protection)) return false;
    const LONG previous = InterlockedCompareExchange(target, desired, expected);
    DWORD ignored{};
    const bool restored = VirtualProtect(const_cast<LONG*>(target), sizeof(LONG), old_protection, &ignored) != FALSE;
    return restored && previous == expected && *target == desired;
}

void locate_god_mode() {
    const auto names = find_string("g_permaGodMode");
    constexpr std::array<std::uint8_t, 40> callback_pattern{
        0x40,0x53,0x48,0x83,0xec,0x20,0x8b,0x41,0x20,0x48,0x8b,0xda,0x83,0xf8,0xff,0x75,
        0x23,0x48,0x8b,0xca,0xe8,0,0,0,0,0x84,0xc0,0x74,0x12,0x48,0x8b,0x05,0,0,0,0,
        0x83,0x78,0x08,0};
    constexpr std::array<std::uint8_t, 40> callback_mask{
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0,0,0,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0,0,0,0,
        0xff,0xff,0xff,0xff};
    const auto callbacks = find_bytes(
        callback_pattern.data(), callback_mask.data(), callback_pattern.size(), true, false);
    std::vector<volatile LONG*> values;
    for (auto* callback : callbacks) {
        const auto displacement = read_at<std::int32_t>(callback + 32);
        auto* global = callback + 36 + displacement;
        auto* record = read_at<std::uint8_t*>(global);
        if (!record) continue;
        auto* value = reinterpret_cast<volatile LONG*>(record + 8);
        if ((*value == 0 || *value == 1) &&
            std::find(values.begin(), values.end(), value) == values.end()) {
            values.push_back(value);
        }
    }
    if (values.size() == 1) g_god_value = values.front();
    dda::log(std::format("God mode CVAR mapping: nameMatches={}, callbackMatches={}, valueMatches={}",
        names.size(), callbacks.size(), values.size()));
}

void locate_noclip_command() {
    constexpr std::array<std::uint8_t, 32> pattern{
        0x48,0x89,0x5c,0x24,0x20,0x57,0x48,0x83,0xec,0x40,0x48,0x8b,0xda,0x48,0x8b,0xf9,
        0x48,0x85,0xd2,0x75,0x43,0x39,0x11,0x7e,0x1e,0x48,0x8b,0x49,0x08,0x48,0x8b,0xd1};
    constexpr std::array<std::uint8_t, 32> mask{
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
    const auto callbacks = find_bytes(pattern.data(), mask.data(), pattern.size(), true, false);
    if (callbacks.size() == 1 && callbacks.front()[97] == 0x83 &&
        callbacks.front()[98] == 0xf8 && callbacks.front()[99] == 0x07 &&
        callbacks.front()[100] == 0x0f && callbacks.front()[101] == 0x85) {
        g_retail_noclip_command = callbacks.front();
    }
    dda::log(std::format("Adaptive retail Noclip mapping: callbackMatches={}, validated={}",
        callbacks.size(), g_retail_noclip_command ? 1 : 0));
}

bool update_noclip_hook() {
    if (!g_retail_noclip_command) return false;
    const bool required =
        g_noclip_feature_enabled.load(std::memory_order_relaxed) ||
        g_noclip_runtime_logging.load(std::memory_order_relaxed);
    if (required == g_noclip_hook_enabled) return true;
    if (!g_noclip_hook_created) {
        if (MH_CreateHook(g_retail_noclip_command,
                reinterpret_cast<void*>(&adaptive_noclip_command),
                &g_original_noclip_command) != MH_OK) {
            dda::log("Adaptive retail Noclip hook creation failed");
            return false;
        }
        g_noclip_hook_created = true;
    }
    const MH_STATUS status = required
        ? MH_EnableHook(g_retail_noclip_command) : MH_DisableHook(g_retail_noclip_command);
    if (status != MH_OK) {
        dda::log(std::format("Adaptive retail Noclip hook toggle failed: status={}",
            static_cast<int>(status)));
        return false;
    }
    g_noclip_hook_enabled = required;
    dda::log(std::format("Adaptive retail Noclip shared hook {}", required ? "enabled" : "disabled"));
    return true;
}

}

namespace dda {
void initialize_cheat_state() {
    std::scoped_lock lock(g_mutex);
    if (g_initialized) return;
    locate_god_mode();
    locate_noclip_command();
    g_initialized = true;
}

bool god_mode_available() {
    std::scoped_lock lock(g_mutex);
    return g_god_value != nullptr;
}

bool god_mode_enabled() {
    std::scoped_lock lock(g_mutex);
    return g_god_value && *g_god_value == 1;
}

bool set_god_mode_enabled(bool enabled) {
    std::scoped_lock lock(g_mutex);
    if (!g_god_value) return false;
    const LONG desired = enabled ? 1 : 0;
    const LONG current = *g_god_value;
    if (current == desired) return true;
    if (current != (enabled ? 0 : 1) || !exchange_long(g_god_value, current, desired)) {
        dda::log("God mode toggle refused or failed validation");
        return false;
    }
    dda::log(std::format("God mode {}", enabled ? "enabled" : "disabled"));
    return true;
}

bool noclip_command_available() {
    std::scoped_lock lock(g_mutex);
    return g_retail_noclip_command != nullptr;
}

bool noclip_command_enabled() {
    return g_noclip_feature_enabled.load(std::memory_order_relaxed);
}

bool set_noclip_command_enabled(bool enabled) {
    std::scoped_lock lock(g_mutex);
    if (!g_retail_noclip_command) return false;
    const bool previous = g_noclip_feature_enabled.exchange(enabled, std::memory_order_relaxed);
    if (previous == enabled) return true;
    if (!update_noclip_hook()) {
        g_noclip_feature_enabled.store(previous, std::memory_order_relaxed);
        return false;
    }
    dda::log(std::format("Adaptive retail Noclip feature {}", enabled ? "enabled" : "disabled"));
    return true;
}

bool noclip_runtime_logging_enabled() {
    return g_noclip_runtime_logging.load(std::memory_order_relaxed);
}

void set_noclip_runtime_logging_enabled(bool enabled) {
    std::scoped_lock lock(g_mutex);
    const bool previous = g_noclip_runtime_logging.exchange(enabled, std::memory_order_relaxed);
    if (previous == enabled) return;
    if (g_retail_noclip_command && !update_noclip_hook()) {
        g_noclip_runtime_logging.store(previous, std::memory_order_relaxed);
        return;
    }
    dda::log(std::format("Noclip runtime debug logging {}", enabled ? "enabled" : "disabled"));
}
}
