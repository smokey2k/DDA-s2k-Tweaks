#include "console_unlock.h"

#include "log.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <vector>

namespace {
constexpr std::array<std::uint8_t, 6> kLocked{0x08, 0x4C, 0x8B, 0x0E, 0xBA, 0x01};
constexpr std::array<std::uint8_t, 6> kUnlocked{0x08, 0x4C, 0x8B, 0x0E, 0xBA, 0x00};
std::uint8_t* g_console_byte{};

template <std::size_t N>
std::vector<std::uint8_t*> find_pattern(const std::array<std::uint8_t, N>& pattern) {
    std::vector<std::uint8_t*> hits;
    const auto module = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    if (!module) return hits;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return hits;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return hits;
    const auto image_size = static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if ((section->Characteristics & (IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE)) == 0) continue;
        const auto offset = static_cast<std::size_t>(section->VirtualAddress);
        const auto section_size = static_cast<std::size_t>(section->Misc.VirtualSize);
        if (offset >= image_size || section_size < N) continue;
        const auto bounded_size = (std::min)(section_size, image_size - offset);
        auto* begin = module + offset;
        for (std::size_t pos = 0; pos + N <= bounded_size; ++pos) {
            bool match = true;
            for (std::size_t j = 0; j < N; ++j) {
                if (begin[pos + j] != pattern[j]) { match = false; break; }
            }
            if (match) hits.push_back(begin + pos);
        }
    }
    return hits;
}
}

namespace dda {
ConsoleUnlockResult unlock_console_commands() {
    const auto locked = find_pattern(kLocked);
    const auto unlocked = find_pattern(kUnlocked);
    log(std::format("Console signature scan: lockedMatches={}, unlockedMatches={}",
                    locked.size(), unlocked.size()));
    if (locked.empty() && unlocked.size() == 1) {
        g_console_byte = unlocked.front() + 5;
        log(std::format("Console commands already unlocked at {}",
                        static_cast<void*>(unlocked.front() + 5)));
        return ConsoleUnlockResult::already_unlocked;
    }
    if (locked.size() != 1 || !unlocked.empty()) {
        log("Console unlock refused because the signature result is not unique");
        return ConsoleUnlockResult::failed;
    }
    auto* target = locked.front() + 5;
    g_console_byte = target;
    if (*target != 0x01) {
        log("Console unlock refused because the target byte is not 0x01");
        return ConsoleUnlockResult::failed;
    }
    DWORD old_protection{};
    if (!VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &old_protection)) {
        log(std::format("Console unlock VirtualProtect failed ({})", GetLastError()));
        return ConsoleUnlockResult::failed;
    }
    *target = 0x00;
    FlushInstructionCache(GetCurrentProcess(), target, 1);
    DWORD ignored{};
    const bool restored = VirtualProtect(target, 1, old_protection, &ignored) != FALSE;
    if (*target != 0x00 || !restored) {
        log(std::format("Console patch verification/protection restore failed ({})", GetLastError()));
        return ConsoleUnlockResult::failed;
    }
    log(std::format("Console commands unlocked at {}", static_cast<void*>(target)));
    return ConsoleUnlockResult::unlocked;
}

bool set_console_commands_unlocked(bool enabled) {
    if (!g_console_byte) return false;
    const std::uint8_t desired = enabled ? 0x00 : 0x01;
    if (*g_console_byte == desired) return true;
    if (*g_console_byte != (enabled ? 0x01 : 0x00)) {
        log("Console toggle refused because the current byte is unexpected");
        return false;
    }
    DWORD old_protection{};
    if (!VirtualProtect(g_console_byte, 1, PAGE_EXECUTE_READWRITE, &old_protection)) return false;
    *g_console_byte = desired;
    FlushInstructionCache(GetCurrentProcess(), g_console_byte, 1);
    DWORD ignored{};
    const bool restored = VirtualProtect(g_console_byte, 1, old_protection, &ignored) != FALSE;
    const bool success = restored && *g_console_byte == desired;
    log(std::format("Console commands {}", success ? (enabled ? "enabled" : "restricted") : "toggle failed"));
    return success;
}

bool console_commands_unlocked() { return g_console_byte && *g_console_byte == 0x00; }
bool console_unlock_available() { return g_console_byte != nullptr; }
}
