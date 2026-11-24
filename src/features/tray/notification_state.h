#pragma once

#include <cstdint>
#include <optional>

namespace waku::tray {

// SHQueryUserNotificationState's QUERY_USER_NOTIFICATION_STATE values (shellapi.h), spelled out so
// no caller needs Win32 headers.
inline constexpr std::int64_t kQunsNotPresent = 1;
inline constexpr std::int64_t kQunsBusy = 2;
inline constexpr std::int64_t kQunsRunningD3dFullScreen = 3;
inline constexpr std::int64_t kQunsPresentationMode = 4;
inline constexpr std::int64_t kQunsAcceptsNotifications = 5;
inline constexpr std::int64_t kQunsQuietTime = 6;
inline constexpr std::int64_t kQunsApp = 7;

struct ScreenRect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
};

// The foreground window, as far as the notification state cares.
struct ForegroundWindow {
  bool exists = false;     // there is one, it is visible and not minimized
  bool ours = false;       // it belongs to waku's own process (a bar, a panel, a launcher)
  bool desktop = false;    // the desktop itself (Progman, WorkerW)
  bool maximized = false;  // IsZoomed: a maximized window overhangs its monitor by its borders
  bool captioned = false;  // the style has WS_CAPTION: an ordinary window, not a full-screen one
  ScreenRect window;       // GetWindowRect
  ScreenRect monitor;      // the full rectangle of its monitor, not the work area
};

// True when `window` is another program running full screen (a video, a borderless game): it covers
// its whole monitor, has no caption, and is not maximized, ours or the desktop.
bool isFullScreenApplication(const ForegroundWindow& window);

// The answer to SHQueryUserNotificationState that the tray host gives.
//
// `explorer` is Explorer's own answer, or nothing when it gave none in range (no Explorer, a hung
// taskbar). Without one: QUNS_ACCEPTS_NOTIFICATIONS, what Explorer says on an ordinary desktop.
//
// Explorer answers QUNS_BUSY whenever a window covers a whole monitor, and Octalia's MainScreen (a
// click-through topmost window over its monitor) is such a window, so Explorer says busy for as long
// as the shell runs. That busy is not passed on: programs that ask (Discord) would drop every
// notification. Busy is kept only when the window in front really is another program running full
// screen. Every other answer of Explorer's (presentation mode, quiet time, Direct3D full screen) is
// passed on as it is.
//
// Explorer itself still sees the MainScreen as full screen, and that is wanted: with "do not disturb
// in full screen" on, it puts the toasts in the notification center without a banner, and the
// shell shows them from there instead.
std::int64_t userNotificationState(std::optional<std::int64_t> explorer,
                                   const ForegroundWindow& foreground);

}  // namespace waku::tray
