#include "notification_state.h"

#include <iostream>
#include <string>

int main() {
    auto& queue = dda::notification_state();
    for (int i = 0; i < 12; ++i)
        queue.show("message " + std::to_string(i), dda::NotificationKind::info, 10.0f);
    if (queue.visible()) return 1;
    queue.set_render_ready();
    const auto notifications = queue.snapshots();
    const bool valid = queue.visible() && notifications.size() == 12 &&
        notifications.front().text == "message 0" && notifications.back().text == "message 11";
    queue.dismiss();
    std::cout << "queued=" << notifications.size() << " ordered=" << valid
              << " dismissed=" << !queue.visible() << '\n';
    return valid && !queue.visible() ? 0 : 1;
}
