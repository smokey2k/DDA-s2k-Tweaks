#include "notification_state.h"

#include <chrono>
#include <algorithm>

namespace dda {
namespace {
std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

void NotificationState::show(std::string_view message, NotificationKind kind,
                             float duration_seconds) {
    const auto duration_ms = static_cast<std::int64_t>(duration_seconds * 1000.0f);
    const auto now = now_ms();
    std::scoped_lock lock(mutex_);
    if (entries_.size() >= 64) entries_.erase(entries_.begin());
    entries_.push_back({kind, std::string(message), duration_ms,
        render_ready_ ? now + duration_ms : 0});
}

void NotificationState::show_console_state_result(bool success, bool unlocked, bool already_unlocked,
                                                  float duration_seconds) {
    if (!success) show("CONSOLE UNLOCK - FAILED (CHECK LOG)", NotificationKind::error, duration_seconds);
    else if (!unlocked) show("CONSOLE COMMANDS - RESTRICTED", NotificationKind::warning, duration_seconds);
    else if (already_unlocked) show("CONSOLE COMMANDS - ALREADY UNLOCKED", NotificationKind::info, duration_seconds);
    else show("CONSOLE COMMANDS - UNLOCKED", NotificationKind::success, duration_seconds);
}

void NotificationState::start_pending_locked(std::int64_t now) {
    for (auto& entry : entries_) {
        if (!entry.visible_until_ms) entry.visible_until_ms = now + entry.duration_ms;
    }
}

void NotificationState::remove_expired_locked(std::int64_t now) {
    std::erase_if(entries_, [now](const Entry& entry) {
        return entry.visible_until_ms && entry.visible_until_ms <= now;
    });
}

void NotificationState::set_render_ready() {
    const auto now = now_ms();
    std::scoped_lock lock(mutex_);
    render_ready_ = true;
    start_pending_locked(now);
}

bool NotificationState::visible() {
    const auto now = now_ms();
    std::scoped_lock lock(mutex_);
    if (!render_ready_) return false;
    start_pending_locked(now);
    remove_expired_locked(now);
    return !entries_.empty();
}

std::vector<NotificationSnapshot> NotificationState::snapshots() {
    const auto now = now_ms();
    std::scoped_lock lock(mutex_);
    if (!render_ready_) return {};
    start_pending_locked(now);
    remove_expired_locked(now);
    std::vector<NotificationSnapshot> result;
    result.reserve(entries_.size());
    for (const auto& entry : entries_) result.push_back({entry.kind, entry.text});
    return result;
}

void NotificationState::dismiss() {
    std::scoped_lock lock(mutex_);
    entries_.clear();
}

NotificationState& notification_state() {
    static NotificationState state;
    return state;
}
}
