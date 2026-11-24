#include "features/tray/notification_state.h"

namespace waku::tray {

bool isFullScreenApplication(const ForegroundWindow& window) {
  if (!window.exists || window.ours || window.desktop || window.maximized || window.captioned) {
    return false;
  }
  const ScreenRect& w = window.window;
  const ScreenRect& m = window.monitor;
  if (m.right <= m.left || m.bottom <= m.top) return false;
  return w.left <= m.left && w.top <= m.top && w.right >= m.right && w.bottom >= m.bottom;
}

std::int64_t userNotificationState(std::optional<std::int64_t> explorer,
                                   const ForegroundWindow& foreground) {
  if (!explorer || *explorer < kQunsNotPresent || *explorer > kQunsApp) {
    return kQunsAcceptsNotifications;
  }
  if (*explorer == kQunsBusy && !isFullScreenApplication(foreground)) {
    return kQunsAcceptsNotifications;
  }
  return *explorer;
}

}  // namespace waku::tray
