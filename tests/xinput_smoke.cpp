#include <Windows.h>
#include <Xinput.h>

#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const HMODULE module = LoadLibraryW(argv[1]);
    if (!module) {
        std::cerr << "LoadLibrary failed: " << GetLastError() << '\n';
        return 3;
    }
    const auto get_state = GetProcAddress(module, "XInputGetState");
    const auto set_state = GetProcAddress(module, "XInputSetState");
    const auto ordinal_2 = GetProcAddress(module, MAKEINTRESOURCEA(2));
    const auto ordinal_3 = GetProcAddress(module, MAKEINTRESOURCEA(3));
    std::cout << "named=" << (get_state && set_state)
              << " ordinals=" << (ordinal_2 && ordinal_3) << '\n';
    Sleep(500); // Let the DLL's initialization worker complete in the smoke host.
    return get_state && set_state && ordinal_2 && ordinal_3 ? 0 : 4;
}
