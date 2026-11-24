#include <gtest/gtest.h>

#include "features/tray/notification_state.h"

using namespace waku::tray;

namespace {

constexpr ScreenRect kMonitor{0, 0, 3440, 1440};

// Another program's borderless window over the whole monitor: a video or a game in full screen.
ForegroundWindow fullScreenApp() {
  ForegroundWindow window;
  window.exists = true;
  window.window = kMonitor;
  window.monitor = kMonitor;
  return window;
}

// An ordinary tiled window: captioned, smaller than its monitor.
ForegroundWindow tiledWindow() {
  ForegroundWindow window;
  window.exists = true;
  window.captioned = true;
  window.window = {8, 48, 1720, 1432};
  window.monitor = kMonitor;
  return window;
}

}  // namespace

TEST(TrayNotificationState, WithoutAnAnswerFromExplorerNotificationsAreAccepted) {
  EXPECT_EQ(userNotificationState(std::nullopt, tiledWindow()), kQunsAcceptsNotifications);
  EXPECT_EQ(userNotificationState(std::nullopt, fullScreenApp()), kQunsAcceptsNotifications);
}

TEST(TrayNotificationState, AnAnswerOutOfRangeCountsAsNone) {
  EXPECT_EQ(userNotificationState(0, tiledWindow()), kQunsAcceptsNotifications);
  EXPECT_EQ(userNotificationState(8, fullScreenApp()), kQunsAcceptsNotifications);
}

// Octalia's MainScreen covers its monitor, so Explorer says busy while the shell runs; Discord then
// dropped every notification.
TEST(TrayNotificationState, BusyFromTheShellsOwnWindowsIsNotPassedOn) {
  EXPECT_EQ(userNotificationState(kQunsBusy, tiledWindow()), kQunsAcceptsNotifications);

  ForegroundWindow ours = fullScreenApp();
  ours.ours = true;
  EXPECT_EQ(userNotificationState(kQunsBusy, ours), kQunsAcceptsNotifications);

  ForegroundWindow none;
  EXPECT_EQ(userNotificationState(kQunsBusy, none), kQunsAcceptsNotifications);
}

TEST(TrayNotificationState, BusyIsKeptForAnotherProgramInFullScreen) {
  EXPECT_EQ(userNotificationState(kQunsBusy, fullScreenApp()), kQunsBusy);

  ForegroundWindow overhanging = fullScreenApp();
  overhanging.window = {-1, -1, 3441, 1441};
  EXPECT_EQ(userNotificationState(kQunsBusy, overhanging), kQunsBusy);
}

TEST(TrayNotificationState, OtherAnswersOfExplorerArePassedOn) {
  for (std::int64_t state : {kQunsNotPresent, kQunsRunningD3dFullScreen, kQunsPresentationMode,
                             kQunsAcceptsNotifications, kQunsQuietTime, kQunsApp}) {
    EXPECT_EQ(userNotificationState(state, tiledWindow()), state) << state;
  }
}

TEST(TrayNotificationState, OnlyABorderlessWindowOverTheWholeMonitorIsFullScreen) {
  EXPECT_TRUE(isFullScreenApplication(fullScreenApp()));
  EXPECT_FALSE(isFullScreenApplication(tiledWindow()));

  ForegroundWindow maximized = fullScreenApp();
  maximized.maximized = true;
  maximized.window = {-8, -8, 3448, 1448};
  EXPECT_FALSE(isFullScreenApplication(maximized));

  ForegroundWindow captioned = fullScreenApp();
  captioned.captioned = true;
  EXPECT_FALSE(isFullScreenApplication(captioned));

  ForegroundWindow desktop = fullScreenApp();
  desktop.desktop = true;
  EXPECT_FALSE(isFullScreenApplication(desktop));

  ForegroundWindow minimized = fullScreenApp();
  minimized.exists = false;
  EXPECT_FALSE(isFullScreenApplication(minimized));

  ForegroundWindow short_ = fullScreenApp();
  short_.window.bottom = 1400;
  EXPECT_FALSE(isFullScreenApplication(short_));

  ForegroundWindow noMonitor = fullScreenApp();
  noMonitor.monitor = {};
  EXPECT_FALSE(isFullScreenApplication(noMonitor));
}
