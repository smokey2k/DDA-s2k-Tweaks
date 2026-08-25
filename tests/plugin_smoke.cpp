#include "plugin_api.h"

#include <Windows.h>

#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::cerr << "usage: plugin_smoke <s2k_Tweaks.dll>\n";
        return 2;
    }
    const HMODULE module = LoadLibraryW(argv[1]);
    if (!module) {
        std::cerr << "LoadLibrary failed: " << GetLastError() << '\n';
        return 1;
    }
    const auto get_api = reinterpret_cast<S2kGetPluginApi>(
        GetProcAddress(module, "s2k_get_plugin_api"));
    const S2kPluginApi* api = get_api ? get_api(S2K_PLUGIN_ABI_VERSION) : nullptr;
    S2kHostApi host{};
    host.abi_version = S2K_PLUGIN_ABI_VERSION;
    const bool valid = api && api->abi_version == S2K_PLUGIN_ABI_VERSION && api->name &&
        api->version && api->initialize && api->update && api->render && api->shutdown &&
        api->initialize(&host);
    if (valid) api->update();
    if (valid) api->shutdown();
    std::cout << "export=" << (get_api != nullptr) << " abi=" << (api != nullptr)
              << " init=" << valid;
    if (api) std::cout << " name=" << api->name << " version=" << api->version;
    std::cout << '\n';
    FreeLibrary(module);
    return valid ? 0 : 1;
}
