#include "log.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace {
std::mutex g_log_mutex;
std::filesystem::path g_log_path;
}

namespace dda {
void initialize_log() {
    wchar_t module_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    g_log_path = std::filesystem::path(module_path).parent_path() / "s2k_Tweaks.log";
}

void log(const std::string_view message) {
    std::scoped_lock lock(g_log_mutex);
    if (g_log_path.empty()) {
        initialize_log();
    }
    std::ofstream output(g_log_path, std::ios::app);
    output << message << '\n';
}
}
