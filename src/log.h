#pragma once

#include <string_view>

namespace dda {
void initialize_log();
void log(std::string_view message);
}
