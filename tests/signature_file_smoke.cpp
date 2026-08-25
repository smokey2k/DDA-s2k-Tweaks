#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <vector>

std::size_t count_pattern(
    const std::vector<std::uint8_t>& bytes, const std::uint8_t* pattern,
    const std::uint8_t* mask, std::size_t length) {
    std::size_t count = 0;
    for (std::size_t i = 0; i + length <= bytes.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < length; ++j) {
            if (mask[j] && bytes[i + j] != pattern[j]) { match = false; break; }
        }
        count += match;
    }
    return count;
}

std::size_t count_string(const std::vector<std::uint8_t>& bytes, std::string_view text) {
    std::vector<std::uint8_t> pattern(text.begin(), text.end());
    pattern.push_back(0);
    std::vector<std::uint8_t> mask(pattern.size(), 0xff);
    return count_pattern(bytes, pattern.data(), mask.data(), pattern.size());
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) return 3;
    const std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(file), {});
    constexpr std::array<std::uint8_t, 6> locked{0x08, 0x4C, 0x8B, 0x0E, 0xBA, 0x01};
    constexpr std::array<std::uint8_t, 6> unlocked{0x08, 0x4C, 0x8B, 0x0E, 0xBA, 0x00};
    constexpr std::array<std::uint8_t, 6> console_mask{0xff,0xff,0xff,0xff,0xff,0xff};
    constexpr std::array<std::uint8_t, 32> retail_noclip{
        0x48,0x89,0x5c,0x24,0x20,0x57,0x48,0x83,0xec,0x40,0x48,0x8b,0xda,0x48,0x8b,0xf9,
        0x48,0x85,0xd2,0x75,0x43,0x39,0x11,0x7e,0x1e,0x48,0x8b,0x49,0x08,0x48,0x8b,0xd1};
    constexpr std::array<std::uint8_t, 32> retail_mask{
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
    constexpr std::array<std::uint8_t, 28> debug_noclip{
        0x48,0x89,0x5c,0x24,0,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x02,0x48,0x8b,0xca,
        0x48,0x8b,0xda,0xff,0x90,0,0,0,0,0x48,0x8b,0xc8};
    constexpr std::array<std::uint8_t, 28> debug_mask{
        0xff,0xff,0xff,0xff,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0,0,0,0,0xff,0xff,0xff};
    const auto locked_count = count_pattern(bytes, locked.data(), console_mask.data(), locked.size());
    const auto unlocked_count = count_pattern(bytes, unlocked.data(), console_mask.data(), unlocked.size());
    const auto retail_count = count_pattern(bytes, retail_noclip.data(), retail_mask.data(), retail_noclip.size());
    const auto debug_count = count_pattern(bytes, debug_noclip.data(), debug_mask.data(), debug_noclip.size());
    const auto noclip_name_count = count_string(bytes, "Noclip");
    const auto noclip_description_count = count_string(
        bytes, "disables collision detection for the player. Specify the player index for non-local players");
    const auto god_cvar_count = count_string(bytes, "g_permaGodMode");
    std::cout << "locked=" << locked_count << " unlocked=" << unlocked_count
              << " retailNoclip=" << retail_count << " debugNoclip=" << debug_count
              << " noclipName=" << noclip_name_count << " noclipDescription=" << noclip_description_count
              << " godCvar=" << god_cvar_count << '\n';
    return locked_count + unlocked_count == 1 && retail_count == 1 && debug_count == 1 &&
        noclip_name_count == 1 && noclip_description_count == 1 && god_cvar_count == 1 ? 0 : 1;
}
