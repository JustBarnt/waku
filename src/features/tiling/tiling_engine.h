#pragma once

#include <QRect>
#include <QString>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "features/tiling/layouts.h"
#include "features/tiling/tiling_events.h"
#include "features/tiling/tiling_model.h"
#include "features/tiling/tiling_rules.h"
#include "features/tiling/window_placer.h"

namespace waku::tiling {

// Runs a task later on the calling thread. The real one is a QTimer::singleShot; tests use a
// manual clock (testing/fake_window_placer.h).
class Scheduler {
 public:
  virtual ~Scheduler() = default;
  virtual void after(int ms, std::function<void()> task) = 0;
};

std::unique_ptr<Scheduler> makeQtScheduler();

// What a title-bar drag of a tiled window does. Float takes it out of the layout; Swap drops it
// where the cursor is released: onto another tile (the two trade places), onto a stack (it joins
// it), onto another monitor (it joins that layout at the end) or back into its own tile.
enum class DragAction { Float, Swap };

struct TilingSettings {
  QString layout = QStringLiteral("mainStack");  // the layout of a workspace with none of its own
  int gap = 8;                                   // pixels between windows and around them
  double ratio = 0.5;                            // the share of the width the main window takes
  std::vector<Rule> rules;
  bool selfElevated = false;  // waku runs elevated: elevated windows are movable
  DragAction dragAction = DragAction::Float;
  ModifierKey dragFloatKey = ModifierKey::Ctrl;  // held at the release of a Swap drag: it floats
};

// Where the window of a Swap drag would land if it were released now. `rect` is in physical
// pixels on `monitor`: the tile it takes (Swap, Stack, Back) or the spot it gets at the end of the
// other monitor's layout (Monitor); empty for Float and None.
enum class DropKind { None, Swap, Stack, Monitor, Float, Back };

struct DropTarget {
  DropKind kind = DropKind::None;
  MonitorId monitor = 0;
  QRect rect;
  WindowId window = 0;  // the window whose tile it is (Swap, Stack)
  friend bool operator==(const DropTarget&, const DropTarget&) = default;
};

struct TilingStatus {
  bool running = false;
  QString desktop;
  int managed = 0;
  int floating = 0;
  int workspaces = 0;
  int stacks = 0;   // slots that hold more than one window
  int clamped = 0;  // placed windows whose bounds differ from their slot (a minimum size)
  QString layout;
  bool paused = false;
};

// The answer of a command: `message` is the text after "ok: " or "error: ".
struct CommandResult {
  bool ok = false;
  QString message;
};

enum class FloatOutcome { NoWindow, Floated, Tiled, Unchanged, Ineligible };

// The tiler: takes window events and keeps every managed window of the shown virtual desktop in
// the slot its layout gives it. Everything it knows about a window comes from the WindowPlacer, so
// the whole behaviour runs against a fake in tests. No thread of its own: the caller feeds it
// events on the GUI thread and the scheduler runs its deferred work on the same thread.
//
// What it does, per event (the hooks send each as it happens):
//  - a window that shows, uncloaks, restores or gets a title is evaluated one event-loop turn
//    later (Show arrives before the final title and size); a window that is still minimized or
//    untitled is looked at again every 100 ms, five times at most
//  - a window that goes away, hides or minimizes leaves its stack
//  - a window that the user moved or resized (MoveSizeEnd with another rectangle) floats
//  - a desktop switch has no event: the first Uncloaked or Foreground after it finds a different
//    current desktop and retiles that desktop
//  - retiles are coalesced: any number of requests in one turn place the windows once
// Startup restores eligible maximized windows before tiling them; later maximization is respected.
// It never activates a window, never places a cloaked, minimized, maximized or fullscreen one, and
// reads the rectangle back after a placement: a window that clamps (minimum size) keeps its slot
// and leaves a gap. The exception is a window a `settle` rule names, right after it opens: an
// application that puts back the bounds it saved when it last closed does so after it shows, over
// the tiler's placement, sometimes in steps (Zen and Firefox: the position, then the size). Within
// kSettleMs, each change it makes restarts a kSettleQuietMs wait; when the wait runs out it is
// placed again, kSettlePlacements times at most.
class TilingEngine {
 public:
  static constexpr int kRetryMs = 100;
  static constexpr int kMaxRetries = 5;
  // How long after a placement the engine reads the window's bounds back: SetWindowPos is posted,
  // so the window applies it a moment later.
  static constexpr int kVerifyMs = 150;
  // A newly managed window under a `settle` rule that moves off its slot by itself within kSettleMs
  // is placed again
  // once it has made no change for kSettleQuietMs, kSettlePlacements times at most; later changes
  // are its own. kSettleMs covers a browser's session restore, which can come seconds after the
  // window shows.
  static constexpr int kSettleMs = 10000;
  static constexpr int kSettleQuietMs = 300;
  static constexpr int kSettlePlacements = 3;

  TilingEngine(WindowPlacer& placer, Scheduler& scheduler);
  ~TilingEngine();
  TilingEngine(const TilingEngine&) = delete;
  TilingEngine& operator=(const TilingEngine&) = delete;

  void setSettings(const TilingSettings& settings);
  const TilingSettings& settings() const { return settings_; }

  // Scans the open windows, restores eligible maximized ones and tiles them. Idempotent.
  void start();
  // Forgets everything and leaves the windows where they are.
  void stop();
  bool running() const { return running_; }

  void handle(const WindowEvent& event);
  // True only while a managed window of a layout that resizes by mouse (resizesByMouse) is being
  // dragged by its border or title bar.
  bool tracksMouseResize(WindowId hwnd) const { return resizeDrags_.count(hwnd) != 0; }
  // The layouts where a border drag resizes the tiles instead of floating the window.
  static bool resizesByMouse(const QString& layout);
  // True while the engine wants the location events of the window: a mainStack resize or a drag
  // under DragAction::Swap.
  bool tracksLocation(WindowId hwnd) const {
    return resizeDrags_.count(hwnd) != 0 || moveDrags_.count(hwnd) != 0;
  }
  // Where the window of a Swap drag would land now; kind None outside one.
  const DropTarget& dropTarget() const { return drop_; }
  // Called each time dropTarget() changes.
  void setDropListener(std::function<void(const DropTarget&)> listener) {
    dropListener_ = std::move(listener);
  }

  // Retiles every workspace of the shown desktop (a screen or work area change).
  void retileAll();

  // Floats or tiles the foreground window: `floating` unset toggles.
  FloatOutcome setForegroundFloating(std::optional<bool> floating);
  // The layout of the workspace of the foreground window (the default layout when no managed
  // window is in front). False for an unknown name.
  bool setWorkspaceLayout(const QString& name);
  // The layout in force for the foreground window's workspace (the default layout without one).
  QString currentLayout() const;
  // Moves the layout of that workspace `step` places along layoutNames(), wrapping. The message
  // is the new layout's name.
  CommandResult cycleLayout(int step);
  // Activates the next or previous window of the foreground window's workspace, wrapping.
  bool focusStep(int direction);

  // The commands on the focused (foreground) window. All but pause, close and minimize need it to
  // be managed (tiled); only `focusDirection` activates a window.
  //
  // Activates the nearest tiled window of the workspace in `direction`, by slot position.
  CommandResult focusDirection(Direction direction);
  // Swaps the focused window with its neighbour in `direction` and retiles.
  CommandResult moveDirection(Direction direction);
  // Gives the focused window the main slot (the front of the stack).
  CommandResult promote();
  // Stacking: several windows share one slot and only the top one shows, the others sit under it.
  // Puts the focused window on the stack of the slot nearest in `direction` (it becomes the top).
  CommandResult stackDirection(Direction direction);
  // Takes the focused window out of its stack into a slot of its own right after the stack's.
  CommandResult unstack();
  // Brings the next (`step` > 0) or previous window of the focused window's stack to the top and
  // activates it.
  CommandResult cycleStack(int step);
  // Grows (`sign` > 0) or shrinks the focused window along `axis` by one step: the ratio
  // (kRatioStep) where the main window's share is along that axis (mainStack, centerMain, ...),
  // otherwise the window's weight against its neighbours (kWeightStep): its width in columns, its
  // height in rows, in the stack of mainStack and in a side column of centerMain. Per workspace;
  // clamped. grid does not resize.
  CommandResult resize(Axis axis, int sign);
  // Mirrors the layout of the focused window's workspace; again undoes it.
  CommandResult flip(Axis axis);
  // The focused window fills the work area of its monitor; the others stay where they are. Again
  // tiles it back.
  CommandResult monocle();
  // Stops placing windows (they are still tracked) and starts again; the second call retiles.
  CommandResult togglePause();
  // Politely closes or minimizes the foreground window, tiled or not (never one of waku, the
  // desktop or the shell).
  CommandResult closeForeground();
  CommandResult minimizeForeground();
  bool paused() const { return paused_; }

  static constexpr double kRatioStep = 0.05;
  static constexpr double kWeightStep = 0.1;
  static constexpr double kMinWeight = 0.3;
  static constexpr double kMaxWeight = 3.0;

  TilingStatus status() const;
  const TilingModel& model() const { return model_; }

 private:
  void defer(int ms, std::function<void()> task);
  void scheduleEvaluate(WindowId hwnd, int ms, int attempt, bool force, bool initialScan = false);
  void evaluate(WindowId hwnd, int attempt, bool force, bool initialScan = false);
  void waitForRestore(WindowId hwnd, int attempt);
  void forget(WindowId hwnd, bool dropFloat);
  void refreshDesktop();
  void requestRetile(const WorkspaceKey& key);
  void flushRetiles();
  void retile(const WorkspaceKey& key);
  void onCloaked(WindowId hwnd);
  void onMoveSizeStart(WindowId hwnd);
  void onMoveSizeEnd(WindowId hwnd);
  void onLocationChange(WindowId hwnd);
  struct MoveDrag;
  // A Swap drag: follows the window and publishes where it would land.
  void updateMoveDrag(WindowId hwnd);
  // Where the window of `drag` lands if it is released now (reads the cursor and the keys).
  DropTarget dropTargetFor(WindowId hwnd, MoveDrag& drag);
  // The rectangle a window dropped on the workspace `key` gets: the last slot of its layout with
  // one window more. Empty when the monitor is unknown.
  QRect spotAtEnd(const WorkspaceKey& key);
  // Applies the drop of a Swap drag that moved the window (same size); false when it floats.
  bool dropMoved(WindowId hwnd, const WorkspaceKey& key, MoveDrag& drag);
  // Ends the Swap drag of the window, if any, and clears the drop target.
  void endMoveDrag(WindowId hwnd);
  void publishDrop(const DropTarget& target);
  // A newly managed window that moved by itself after its placement goes back to its slot.
  void onSettlingMove(WindowId hwnd);
  void raiseNewTops(const WorkspaceKey& key, const std::vector<WindowId>& active);
  void verifyPlacements(const std::vector<std::pair<WindowId, QRect>>& placed);
  // Starts the settling period of a window the engine has just started managing.
  void startSettling(WindowId hwnd);
  // When `hwnd` is settling and has placements left: (re)starts its quiet wait, at the end of which
  // it is placed again, and returns true. `why` is for the debug line.
  bool settleLater(WindowId hwnd, const WindowFacts& facts, const QString& why);
  WorkspaceKey keyFor(const WindowFacts& facts) const;
  std::optional<WorkspaceKey> foregroundKey() const;
  struct Focused {
    WorkspaceKey key;
    WindowId hwnd = 0;
  };
  std::optional<Focused> focusedManaged() const;
  // The windows of the stack that take a slot now, in order (no one is forgotten here).
  std::vector<WindowId> activeWindows(const WorkspaceKey& key);
  // The slots of `active` (the layout with the workspace's ratio, weights and flips).
  std::vector<QRect> slotsFor(const WorkspaceKey& key, const QRect& area,
                              const std::vector<WindowId>& active) const;
  // The foreground window when a command may close or minimize it.
  std::optional<WindowId> actionableForeground(QString* error);

  WindowPlacer& placer_;
  Scheduler& scheduler_;
  TilingSettings settings_;
  TilingModel model_;
  bool running_ = false;
  bool paused_ = false;
  QString currentDesktop_;
  // Windows that were looked at and rejected, and why. A later Show looks again unless the verdict
  // is Lasting; a title change looks again only at an Untitled one.
  enum class Ignored { Lasting, Untitled, Transient };
  std::map<WindowId, Ignored> ignored_;
  std::set<WindowId> pending_;  // an evaluation is scheduled
  // Maximized at startup or explicitly requested back into tiling. Cloaked windows wait.
  std::set<WindowId> restoreBeforeTile_;
  std::set<WindowId> moving_;         // the user drags or resizes it
  std::map<WindowId, QRect> placed_;  // the visible bounds the window had when we last looked
  // The size a window gets back when toggle or float takes it out of the layout: the visible bounds
  // it had before the tiler first placed it (its launch size, or its size when waku started), then,
  // each time it goes from floating back to tiled, the size it had while floating. Kept while the
  // window lives, through floats, minimizes and desktop moves; dropped when it is destroyed.
  std::map<WindowId, QRect> floatRects_;
  struct ResizeDrag {
    WorkspaceKey key;
    QString layout;  // canonical, at the start
    // Both edges of an axis moved at once: Windows placed it (a Snap at the release). It floats.
    bool snapped = false;
    QRect startRect;
    QRect area;
    std::vector<WindowId> active;
    std::vector<QRect> slotRects;
    std::optional<double> initialRatio;
    std::map<WindowId, double> initialWeights;
    int gap = 0;
    bool resized = false;
  };
  std::map<WindowId, ResizeDrag> resizeDrags_;
  // A title-bar drag under DragAction::Swap: the workspace's tiles when it started. It ends at
  // MoveSizeEnd, or early when the window changes size (a resize, not a move).
  struct MoveDrag {
    WorkspaceKey key;
    QRect startRect;
    std::vector<WindowId> active;
    std::vector<QRect> slotRects;
    // Left its tile: the drop target is shown from then on.
    bool moved = false;
    // spotAtEnd per other monitor, worked out once per drag.
    std::map<MonitorId, QRect> spots;
  };
  std::map<WindowId, MoveDrag> moveDrags_;
  // Dropped on another monitor: its next placement puts it there, whichever monitor holds most of
  // it until then (retile would otherwise send it back).
  std::set<WindowId> dropped_;
  DropTarget drop_;
  std::function<void(const DropTarget&)> dropListener_;
  std::set<WindowId> clamped_;  // placed, but not at the size of its slot
  // Newly managed windows: the placements left to undo a change of their own bounds, the serial of
  // the settling period (a later period of the same window outlives this one's timer) and of the
  // quiet wait now running (a newer change makes the older wait do nothing).
  struct Settling {
    int left = kSettlePlacements;
    unsigned period = 0;
    unsigned quiet = 0;
  };
  std::map<WindowId, Settling> settling_;
  unsigned settleSerial_ = 0;
  // Placed, and the readback has not run yet: the bounds seen until then can be stale.
  std::set<WindowId> verifying_;
  std::set<WindowId> wasTop_;   // tops of stacks (two windows or more) at the last retile
  std::set<WorkspaceKey> retiles_;
  bool flushScheduled_ = false;
  // Tasks hold a weak copy: a task that outlives stop() or the engine does nothing.
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace waku::tiling
