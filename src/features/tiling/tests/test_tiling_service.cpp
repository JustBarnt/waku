// The Tiling service on fakes: the switch, the settings, the commands and the capability gate. No
// real window or hook.
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QVariantMap>

#include "features/tiling/qml/tiling_service.h"
#include "features/tiling/testing/fake_window_placer.h"
#include "kernel/qt/live_desktop.h"

using namespace waku::tiling;
using waku::tiling::testing::FakeScheduler;
using waku::tiling::testing::FakeWindowPlacer;

namespace {

class FakeHooks final : public TilingHooks {
 public:
  bool install(Sink sink) override {
    ++installs;
    sink_ = std::move(sink);
    return allow;
  }
  void uninstall() override {
    ++uninstalls;
    sink_ = nullptr;
  }
  bool installed() const { return static_cast<bool>(sink_); }
  void trackLocation(WindowId hwnd, bool enabled) override {
    if (enabled) {
      tracked = hwnd;
    } else if (tracked == hwnd || hwnd == 0) {
      tracked = 0;
    }
  }
  void fire(EventKind kind, WindowId hwnd) {
    if (sink_) sink_({kind, hwnd});
  }
  int installs = 0;
  int uninstalls = 0;
  WindowId tracked = 0;  // the window whose location events are wanted
  bool allow = true;

 private:
  Sink sink_;
};

struct Rig {
  Rig() {
    Tiling::Deps deps;
    auto fakePlacer = std::make_unique<FakeWindowPlacer>();
    auto fakeHooks = std::make_unique<FakeHooks>();
    auto fakeScheduler = std::make_unique<FakeScheduler>();
    placer = fakePlacer.get();
    hooks = fakeHooks.get();
    scheduler = fakeScheduler.get();
    deps.placer = std::move(fakePlacer);
    deps.hooks = std::move(fakeHooks);
    deps.scheduler = std::move(fakeScheduler);
    deps.watchScreens = false;
    tiling = std::make_unique<Tiling>(std::move(deps), nullptr);
    tiling->setGap(10);
    tiling->setRatio(0.5);
  }

  FakeWindowPlacer* placer = nullptr;
  FakeHooks* hooks = nullptr;
  FakeScheduler* scheduler = nullptr;
  std::unique_ptr<Tiling> tiling;
};

}  // namespace

TEST(TilingService, IsOffUntilEnabled) {
  Rig rig;
  rig.placer->add(1);
  EXPECT_FALSE(rig.tiling->enabled());
  EXPECT_FALSE(rig.tiling->active());
  EXPECT_FALSE(rig.hooks->installed());
  rig.scheduler->runAll();
  EXPECT_TRUE(rig.placer->placements.empty());
}

TEST(TilingService, EnablingHooksTheEventsAndTilesTheOpenWindows) {
  Rig rig;
  rig.placer->add(1);
  rig.placer->add(2);
  QSignalSpy spy(rig.tiling.get(), &Tiling::activeChanged);
  rig.tiling->setEnabled(true);
  EXPECT_TRUE(rig.tiling->active());
  EXPECT_TRUE(rig.hooks->installed());
  EXPECT_EQ(spy.count(), 1);
  rig.scheduler->runAll();
  EXPECT_EQ(rig.placer->slotsOf(1).size(), 1u);
  EXPECT_EQ(rig.placer->slotsOf(2).size(), 1u);
}

TEST(TilingService, ADisabledTilerUnhooksAndStopsTouchingWindows) {
  Rig rig;
  rig.tiling->setEnabled(true);
  rig.tiling->setEnabled(false);
  EXPECT_FALSE(rig.tiling->active());
  EXPECT_FALSE(rig.hooks->installed());
  const size_t placements = rig.placer->placements.size();
  rig.placer->add(1);
  rig.hooks->fire(EventKind::Show, 1);
  rig.scheduler->runAll();
  EXPECT_EQ(rig.placer->placements.size(), placements);
}

TEST(TilingService, HookEventsReachTheEngine) {
  Rig rig;
  rig.tiling->setEnabled(true);
  rig.placer->add(7);
  rig.hooks->fire(EventKind::Show, 7);
  rig.scheduler->runAll();
  EXPECT_EQ(rig.placer->slotsOf(7).size(), 1u);
}

TEST(TilingService, ARefusedHookLeavesTheTilerInactive) {
  Rig rig;
  rig.hooks->allow = false;
  rig.tiling->setEnabled(true);
  EXPECT_FALSE(rig.tiling->active());
}

TEST(TilingService, WithoutAPlacerTheServiceIsInert) {
  Tiling::Deps deps;
  deps.watchScreens = false;
  Tiling tiling(std::move(deps), nullptr);
  tiling.setEnabled(true);
  EXPECT_FALSE(tiling.active());
  EXPECT_TRUE(tiling.controlStatus().startsWith(QLatin1String("ok: enabled=yes; active=no")));
  EXPECT_TRUE(tiling.controlRetile().startsWith(QLatin1String("error:")));
}

TEST(TilingService, TheDefaultServiceIsInertOnTheOffscreenPlatform) {
  EXPECT_FALSE(waku::kernel::liveDesktop(QStringLiteral("tiling")));
  Tiling tiling;
  tiling.setEnabled(true);
  EXPECT_FALSE(tiling.active());
  EXPECT_EQ(tiling.engine(), nullptr);
}

TEST(TilingService, TheNoTilingSwitchTurnsTheCapabilityOff) {
  waku::kernel::setCapabilityDisabled(QStringLiteral("tiling"), true);
  EXPECT_FALSE(waku::kernel::liveDesktop(QStringLiteral("tiling")));
  EXPECT_TRUE(waku::kernel::capabilityDisabled(QStringLiteral("tiling")));
  waku::kernel::setCapabilityDisabled(QStringLiteral("tiling"), false);
}

TEST(TilingService, SettingsAreValidatedAndNotified) {
  Rig rig;
  QSignalSpy layout(rig.tiling.get(), &Tiling::layoutChanged);
  rig.tiling->setLayoutName(QStringLiteral("Columns"));
  EXPECT_EQ(rig.tiling->layout(), QStringLiteral("columns"));
  EXPECT_EQ(layout.count(), 1);
  rig.tiling->setLayoutName(QStringLiteral("spiral"));  // unknown: ignored
  EXPECT_EQ(rig.tiling->layout(), QStringLiteral("columns"));
  rig.tiling->setGap(-5);
  EXPECT_EQ(rig.tiling->gap(), 0);
  rig.tiling->setRatio(7.0);
  EXPECT_DOUBLE_EQ(rig.tiling->ratio(), 0.9);
  EXPECT_EQ(rig.tiling->layouts(),
            (QStringList{QStringLiteral("mainStack"), QStringLiteral("mainStackTop"),
                         QStringLiteral("columns"), QStringLiteral("rows"), QStringLiteral("bsp"),
                         QStringLiteral("grid"), QStringLiteral("centerMain")}));
  EXPECT_DOUBLE_EQ(TilingSettings().ratio, 0.5);
}

TEST(TilingService, DragSettingsAreValidatedAndNotified) {
  Rig rig;
  QSignalSpy action(rig.tiling.get(), &Tiling::dragActionChanged);
  QSignalSpy key(rig.tiling.get(), &Tiling::dragFloatModifierChanged);
  EXPECT_EQ(rig.tiling->dragAction(), QStringLiteral("float"));
  EXPECT_FALSE(rig.tiling->resizePreview().value(QStringLiteral("active")).toBool());
  EXPECT_EQ(rig.tiling->dragFloatModifier(), QStringLiteral("ctrl"));
  rig.tiling->setDragAction(QStringLiteral("Swap"));
  EXPECT_EQ(rig.tiling->dragAction(), QStringLiteral("swap"));
  rig.tiling->setDragAction(QStringLiteral("throw"));  // unknown: ignored
  EXPECT_EQ(rig.tiling->dragAction(), QStringLiteral("swap"));
  rig.tiling->setDragFloatModifier(QStringLiteral("ALT"));
  EXPECT_EQ(rig.tiling->dragFloatModifier(), QStringLiteral("alt"));
  rig.tiling->setDragFloatModifier(QStringLiteral("win"));  // unknown: ignored
  EXPECT_EQ(rig.tiling->dragFloatModifier(), QStringLiteral("alt"));
  EXPECT_EQ(action.count(), 1);
  EXPECT_EQ(key.count(), 1);
  ASSERT_NE(rig.tiling->engine(), nullptr);
  EXPECT_EQ(rig.tiling->engine()->settings().dragAction, DragAction::Swap);
  EXPECT_EQ(rig.tiling->engine()->settings().dragFloatKey, ModifierKey::Alt);
}

TEST(TilingService, ASwapDragPublishesItsDropTargetAndTracksLocation) {
  Rig rig;
  rig.tiling->setDragAction(QStringLiteral("swap"));
  rig.placer->add(1);
  rig.placer->add(2);
  rig.tiling->setEnabled(true);
  rig.scheduler->runAll();
  EXPECT_EQ(rig.tiling->dropTarget().value(QStringLiteral("active")).toBool(), false);
  QSignalSpy changed(rig.tiling.get(), &Tiling::dropTargetChanged);

  rig.hooks->fire(EventKind::MoveSizeStart, 2);
  EXPECT_EQ(rig.hooks->tracked, 2u);
  const QRect main = rig.placer->windows[1].rect;
  rig.placer->windows[2].rect.moveCenter(main.center());
  rig.placer->cursor = main.center();
  rig.hooks->fire(EventKind::LocationChange, 2);
  EXPECT_EQ(rig.tiling->dropTarget().value(QStringLiteral("kind")).toString(),
            QStringLiteral("swap"));
  EXPECT_GE(changed.count(), 1);

  rig.hooks->fire(EventKind::MoveSizeEnd, 2);
  EXPECT_EQ(rig.hooks->tracked, 0u);
  EXPECT_EQ(rig.tiling->dropTarget().value(QStringLiteral("active")).toBool(), false);
  EXPECT_EQ(rig.tiling->dropTarget().value(QStringLiteral("kind")).toString(), QString());
}

TEST(TilingService, TheCurrentLayoutFollowsADesktopSwitch) {
  Rig rig;
  rig.placer->add(1);
  rig.placer->add(2);
  rig.tiling->setEnabled(true);
  rig.scheduler->runAll();
  rig.placer->front = 1;
  ASSERT_TRUE(rig.tiling->setLayout(QStringLiteral("columns")));
  EXPECT_EQ(rig.tiling->currentLayout(), QStringLiteral("columns"));
  QSignalSpy changed(rig.tiling.get(), &Tiling::currentLayoutChanged);

  // An empty desktop: no window takes the focus, so no Foreground event comes.
  rig.placer->desktop = QStringLiteral("{D2}");
  for (WindowId id : {1, 2}) {
    rig.placer->windows[id].cloaked = true;
    rig.hooks->fire(EventKind::Cloaked, id);
  }
  rig.scheduler->runAll();
  EXPECT_EQ(rig.tiling->currentLayout(), QStringLiteral("mainStack"));
  EXPECT_EQ(changed.count(), 1);
}

TEST(TilingService, RulesFromTheShellReachTheEngine) {
  Rig rig;
  rig.placer->add(1, QStringLiteral("skip.exe"));
  rig.placer->add(2, QStringLiteral("keep.exe"));
  rig.tiling->setRules({QVariantMap{{QStringLiteral("process"), QStringLiteral("skip.exe")},
                                    {QStringLiteral("action"), QStringLiteral("ignore")}}});
  rig.tiling->setEnabled(true);
  rig.scheduler->runAll();
  EXPECT_TRUE(rig.placer->slotsOf(1).empty());
  EXPECT_EQ(rig.placer->slotsOf(2).size(), 1u);
}

TEST(TilingService, ScreenChangesRetileAfterTheDebounce) {
  Rig rig;
  rig.placer->add(1);
  rig.tiling->setEnabled(true);
  rig.scheduler->runAll();
  rig.placer->areas[waku::tiling::testing::kMonitorA] = QRect(0, 50, 1920, 990);
  rig.tiling->screensChanged();
  rig.tiling->screensChanged();  // a burst: one timer
  QTest::qWait(500);
  rig.scheduler->runAll();
  EXPECT_EQ(rig.placer->lastSlot(1).y(), 60);
}

TEST(TilingService, CommandsDriveTheEngineAndAnswerLines) {
  Rig rig;
  rig.placer->add(1);
  rig.placer->add(2);
  EXPECT_TRUE(
      rig.tiling->controlFloat(std::nullopt).startsWith(QLatin1String("error: tiling is off")));
  rig.tiling->setEnabled(true);
  rig.scheduler->runAll();

  rig.placer->front = 2;
  EXPECT_EQ(rig.tiling->controlFloat(std::nullopt), QStringLiteral("ok: floating"));
  rig.scheduler->runAll();
  EXPECT_EQ(rig.tiling->controlFloat(std::nullopt), QStringLiteral("ok: tiled"));
  EXPECT_EQ(rig.tiling->controlLayout(QStringLiteral("columns")),
            QStringLiteral("ok: layout columns"));
  EXPECT_TRUE(rig.tiling->controlLayout(QStringLiteral("spiral"))
                  .startsWith(QLatin1String("error: unknown layout")));
  EXPECT_EQ(rig.tiling->controlLayoutStep(1), QStringLiteral("ok: layout rows"));
  EXPECT_EQ(rig.tiling->controlLayoutStep(-1), QStringLiteral("ok: layout columns"));
  EXPECT_TRUE(rig.tiling->controlStatus().contains(QLatin1String("layout=columns;")));
  EXPECT_EQ(rig.tiling->controlFocus(1), QStringLiteral("ok: focused"));
  EXPECT_EQ(rig.tiling->controlRetile(), QStringLiteral("ok: retiling"));
  const QString status = rig.tiling->controlStatus();
  EXPECT_TRUE(status.contains(QLatin1String("active=yes"))) << status.toStdString();
  EXPECT_TRUE(status.contains(QLatin1String("managed=2"))) << status.toStdString();
}

TEST(TilingService, CommandsExplainWhyTheTilerIsNotRunning) {
  Rig rig;
  EXPECT_EQ(rig.tiling->controlRetile(),
            QStringLiteral("error: tiling is off (set Tiling.enabled in the shell)"));
}
