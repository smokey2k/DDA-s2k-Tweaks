#include "log.h"
#include "plugin_manager.h"
#include "vulkan_hook.h"

#include <Windows.h>

#include <cstdint>

namespace {
DWORD WINAPI initialize(LPVOID) {
    dda::initialize_log();
    dda::log("s2k_Tweaks loaded");
    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (!image || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.TimeDateStamp != 0x6a627f03 ||
        nt->OptionalHeader.SizeOfImage != 0x0b748000) {
        dda::log("Unsupported executable version; initialization stopped");
        return 0;
    }
    if (!GetModuleHandleW(L"vulkan-1.dll")) {
        if (!LoadLibraryW(L"vulkan-1.dll")) {
            dda::log("Proactive Vulkan loader initialization failed");
            return 0;
        }
    }
    if (!dda::install_vulkan_hook()) return 0;
    if (!dda::initialize_plugin_manager()) {
        dda::log("Reloadable addon was not available during core initialization");
    }
    return 0;
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (const HANDLE thread = CreateThread(nullptr, 0, initialize, nullptr, 0, nullptr)) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
