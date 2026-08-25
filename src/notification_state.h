#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace dda {
enum class NotificationKind : std::uint32_t { info, success, warning, error };

struct NotificationSnapshot {
    NotificationKind kind{NotificationKind::info};
    std::string text;
};

class NotificationState {
public:
    void show(std::string_view message, NotificationKind kind, float duration_seconds);
    void show_console_state_result(bool success, bool unlocked, bool already_unlocked = false,
                                   float duration_seconds = 3.5f);
    void set_render_ready();
    [[nodiscard]] bool visible();
    [[nodiscard]] std::vector<NotificationSnapshot> snapshots();
    void dismiss();

private:
    struct Entry {
        NotificationKind kind{NotificationKind::info};
        std::string text;
        std::int64_t duration_ms{};
        std::int64_t visible_until_ms{};
    };
    void start_pending_locked(std::int64_t now);
    void remove_expired_locked(std::int64_t now);
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    bool render_ready_{};
};

NotificationState& notification_state();
}
