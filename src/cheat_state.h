#pragma once

namespace dda {
void initialize_cheat_state();
bool god_mode_available();
bool god_mode_enabled();
bool set_god_mode_enabled(bool enabled);
bool noclip_command_available();
bool noclip_command_enabled();
bool set_noclip_command_enabled(bool enabled);
bool noclip_runtime_logging_enabled();
void set_noclip_runtime_logging_enabled(bool enabled);
}
