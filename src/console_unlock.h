#pragma once

namespace dda {
enum class ConsoleUnlockResult { unlocked, already_unlocked, failed };
ConsoleUnlockResult unlock_console_commands();
bool set_console_commands_unlocked(bool enabled);
bool console_commands_unlocked();
bool console_unlock_available();
}
