#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) return 3;
    const std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(file), {});
    constexpr std::array<std::uint8_t, 6> locked{0x08, 0x4C, 0x8B, 0x0E, 0xBA, 0x01};
    constexpr std::array<std::uint8_t, 6> unlocked{0x08, 0x4C, 0x8B, 0x0E, 0xBA, 0x00};
    std::size_t locked_count = 0, unlocked_count = 0;
    for (std::size_t i = 0; i + locked.size() <= bytes.size(); ++i) {
        bool l = true, u = true;
        for (std::size_t j = 0; j < locked.size(); ++j) {
            l &= bytes[i + j] == locked[j];
            u &= bytes[i + j] == unlocked[j];
        }
        locked_count += l;
        unlocked_count += u;
    }
    std::cout << "locked=" << locked_count << " unlocked=" << unlocked_count << '\n';
    return locked_count + unlocked_count == 1 ? 0 : 1;
}
